# Design notes

How the controller works inside, and why it is built this way.

## Threads

| Thread | Code | Job |
|---|---|---|
| main | `main.c` | start everything, then `sigwait()` for Ctrl+C / SIGTERM, then shut down in order |
| supervisor | `instances.c` | `waitpid(-1)` in a loop: learns the moment any instance exits |
| monitor | `instances.c` | every second: restart crashed instances whose backoff is over, then `GET /health` on each instance |
| proxy accept | `proxy.c` | accepts user connections and queues them |
| proxy workers (16) | `proxy.c`, `thread_pool.c` | forward one request each to an instance and stream the answer back |
| api | `api.c` | serves `deployctl`, one request at a time (requests are rare and fast) |
| deployment | `deployer.c` | one per deployment: canary → rolling update → (rollback) |

## Shared state and locks

There are three locks. Each one protects one thing:

| Lock | Protects |
|---|---|
| `instances_lock` (+ condition variable `instances_changed`) | the instance table: state, pid, traffic counters of every instance |
| `deploy_lock` | stable/previous version, the current deployment, history |
| `events_lock` | the event ring buffer and `events.log` |

**Lock order:** a thread may take `events_lock` while holding either of the
others (to log something), but never the other way round, and no thread holds
`instances_lock` and `deploy_lock` together. With a fixed order there can be no
cycle of threads waiting on each other, so there is no deadlock.

**Waiting for something to happen.** The deployer often has to wait ("until the
canary is ready", "until in-flight requests reach 0"). It holds
`instances_lock` and calls `pthread_cond_timedwait(&instances_changed, ...)`,
which releases the lock while sleeping. Every change to the table (a health
check result, a finished request, a process exit) broadcasts `instances_changed`,
so the deployer wakes up, re-checks its condition, and goes back to sleep if it
isn't met yet. The loop handles spurious wake-ups and timeouts in the same way.

**Abort flag.** `abort_requested` is checked inside loops that hold
`instances_lock`, but it's written by the API thread. Instead of taking a
second lock there (and creating a lock-order question), it is an `atomic_int`.

**Copies, not pointers.** The instance table is a fixed-size array, so pointers
into it stay valid. Anything that has to leave the lock, like the health
checker's list of ports to probe, is copied first. Health checks are network calls
with a 1-second timeout, and holding the lock during them would freeze the
load balancer.

## Starting an instance: `fork` + `exec`

`spawn()` in `instances.c` does this:

1. **Before `fork()`**, prepare everything: open the log file and build the argument strings.
   In a program with threads, the child of `fork()` contains only the calling thread.
   If another thread held a lock (for example inside `malloc`), that lock stays locked forever
   in the child. So between `fork()` and `exec()` the child may only call
   *async-signal-safe* functions: `setpgid`, `dup2`, `close`, `signal`, `sigprocmask`, `execl`, `_exit`.
2. **In the child:**
   - `setpgid(0, 0)` puts the instance in its own process group. Ctrl+C in the terminal goes to the
     foreground process group; without this, every instance would receive SIGINT too and die
     before the controller could stop them gracefully.
   - `dup2(log_fd, 1)` and `dup2(log_fd, 2)` send stdout and stderr to `data/logs/instance-<port>.log`.
   - Close every other file descriptor. Otherwise the child inherits the controller's listening
     sockets, and port 8080 would stay in use after the controller exits.
   - Reset what `exec` would otherwise keep: the blocked-signal mask and the ignored SIGPIPE.
   - `execl()` the release script. If that fails, `_exit(127)` (not `exit`, which would run the
     parent's atexit handlers and flush its stdio buffers a second time).
3. **In the controller:** `setpgid(pid, pid)` again, like shells do, so the group exists no matter
   which process runs first. Then record the pid. This happens while holding `instances_lock`, so the
   supervisor cannot handle the child's exit before its pid is in the table.

## Noticing crashes: `waitpid` and zombies

When a child exits, the kernel keeps its exit status until the parent collects
it with `waitpid()`. Until then the dead child is a *zombie*. The supervisor
thread blocks in `waitpid(-1, &status, 0)`. When it returns, the supervisor looks the
pid up in the table:

- If the instance was `STOPPING`, this is the exit we asked for.
- Otherwise it **crashed**. `WIFEXITED`/`WEXITSTATUS` or `WIFSIGNALED`/`WTERMSIG` tell how
  (for example "killed by signal 6: Abort trap" for `v4`). It is marked `CRASHED` with a restart
  time 1, 2, 4, 8, 16, then 30 seconds away, so a crash-looping release doesn't burn CPU.

## Instance states

```
            spawn                first health check OK
  EMPTY ───────────► STARTING ─────────────────────────► READY ◄──┐
    ▲                    │                                 │      │ check OK
    │                    │ no OK check in 15 s → SIGKILL   │ 3 failed checks
    │                    ▼                                 ▼      │
    │                 CRASHED ◄────── process died ───── UNHEALTHY┘
    │                    │                                 │ 10 failed checks → SIGKILL
    │                    └── backoff over → spawn again ──►STARTING
    │
    └──── instance_stop(): STOPPING → drain → SIGTERM → (SIGKILL) → EMPTY
```

Only `READY` instances receive traffic. This is the same split Kubernetes makes
between readiness (should it get traffic?) and liveness (should it be restarted?).

## Stopping an instance gracefully

`instance_stop()` follows the Kubernetes pod termination sequence:

1. `state = STOPPING`, so the load balancer stops picking it.
2. Wait (up to 5 s) for `in_flight` to reach 0, so requests already in progress finish.
3. `kill(-pgid, SIGTERM)`: ask the whole process group to exit. `demo-service` finishes its
   current request and exits with code 0.
4. If it is still alive after 5 s, `SIGKILL`.

## The load balancer

- **Round-robin** over `READY` instances. The canary is one of them, so with 3 old instances it
  gets about a quarter of the traffic.
- It counts `requests` and `errors` (HTTP 5xx, or no answer) per instance. These counters are
  what the deployer uses to judge a canary.
- **Retries.** If an instance refuses the connection (it just crashed), the request goes to the
  next instance, so the user never notices. A `GET` that got no answer at all is retried too,
  because GET requests have no side effects and can safely be sent twice. A `POST` is not retried:
  the instance may already have acted on it.
- **Bounded thread pool.** The accept thread is the producer and 16 workers are the consumers of
  a queue with room for 256 connections. When the queue is full, the accept thread waits, and new
  connections wait in the kernel's listen backlog. Memory and thread count stay fixed under a
  traffic spike.
- Headers and body are written with **one `send()`**. Two small writes can trigger Nagle's
  algorithm together with delayed ACKs and add about 40 ms to every response.

## Signals in a multi-threaded program

Signal handlers can run in any thread, at any moment, and may only call
async-signal-safe functions. The controller avoids handlers entirely:

1. At the very start, `main` resets SIGINT/SIGTERM to their default disposition. A program
   started in the background by a script inherits SIGINT as *ignored*, and an ignored signal is
   discarded even if we wait for it.
2. It blocks both signals *before* creating any thread, and new threads inherit the mask.
3. After startup, `main` calls `sigwait()`, which waits for one of them like an ordinary function
   call. Shutdown then runs as normal code.
4. `SIGPIPE` is ignored, so writing to a client that hung up returns an error instead of
   terminating the controller.

**Shutdown order matters:** stop accepting new work (load balancer and API), let
a running deployment finish its rollback, stop the monitor so it doesn't
restart anything, stop every instance while the supervisor is still there to
collect them, and finally stop the supervisor.

## Saving state

- `data/state.txt` (stable version, previous version, next deployment id) is replaced
  **atomically**: write `state.txt.tmp`, `fsync()` it so it is really on disk, then `rename()`
  it over the old file. `rename()` is atomic, so after a crash the file is either the old or the
  new version, never half-written.
- `data/history.txt` is **append-only**, one line per finished deployment, `fsync`'d after each write.
- On restart the controller reads both, so it brings back the version that was last deployed
  successfully, not the one it started with originally.

Not covered: `fsync` on the directory after `rename` (needed on some file systems for the rename
itself to survive a power cut), and on macOS `fsync` doesn't force the drive's own cache
(`F_FULLFSYNC` does).

## Common questions

**Why processes instead of containers or Kubernetes?** A container is a process
with extra isolation (namespaces, cgroups). Kubernetes ends up starting, probing,
restarting and signalling processes, and this project does those steps directly so
they can be seen. The parts map like this:

| Here | Kubernetes / CD tools |
|---|---|
| `spawn()`, supervisor restarts | kubelet, `restartPolicy` |
| `GET /health`, READY vs UNHEALTHY | readiness and liveness probes |
| load balancer over READY instances | Service / kube-proxy endpoints |
| rolling update in `roll_out()` | Deployment rolling update |
| canary + error-rate check + rollback | Harness CD canary with Continuous Verification, Argo Rollouts |
| SIGTERM, wait, SIGKILL | `terminationGracePeriodSeconds` |

**What if the controller itself crashes?** The instances keep running (they are
in their own process groups), but nothing supervises them or routes traffic to
them, and they still hold their ports. They have to be stopped (`pkill demo-service`)
before the controller starts again; it then starts fresh instances of the last good
version. A production system would re-adopt running instances instead, and replicate
its control plane so it isn't a single point of failure.

**Why `202 Accepted` for `/deploy`?** A deployment takes many seconds. The API
starts it on its own thread and answers right away. The client follows progress
with `GET /status`, a common pattern for long-running operations in REST APIs.

**Why one global instance lock?** Every operation on the table is a few
microseconds. Nothing slow (network calls, `fork`) happens while it's held
except `spawn()`, and `spawn()` only runs during deployments and restarts. Profiling
would have to show contention before finer-grained locking is worth its complexity.
