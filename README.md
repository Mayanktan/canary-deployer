# canary-deployer

[![CI](https://github.com/Mayanktan/canary-deployer/actions/workflows/ci.yml/badge.svg)](https://github.com/Mayanktan/canary-deployer/actions/workflows/ci.yml)
![C](https://img.shields.io/badge/language-C11-blue)
![POSIX](https://img.shields.io/badge/runs%20on-Linux%20%7C%20macOS-lightgrey)

A deployment controller written in C. It runs several copies of a web service,
load-balances traffic across them, keeps them healthy, and rolls out new
versions safely: **canary first, then a rolling update, with an automatic
rollback** when the new version misbehaves.

It is a small, from-scratch version of what Kubernetes Deployments and canary
tools like Harness CD or Argo Rollouts do, built on plain operating-system
primitives (processes, signals, sockets, threads) so every step is visible.

## The problem

Shipping a new version to every server at once is risky: if it has a bug,
every user hits it. A **canary release** first starts the new version on one
instance, so it receives only a small share of real traffic, and watches it:

- healthy → replace the old instances one at a time (no downtime)
- errors or crashes → **remove it automatically**; only that small share of users ever saw the problem

Health checks alone are not enough. A buggy release can answer `GET /health`
perfectly and still fail real requests, so the controller judges the canary on
the **real traffic** it serves.

## What it looks like

A buggy release (`v3`, fails 40% of requests) is caught about one second after it goes live:

```
$ ./bin/deployctl deploy v3
deployment #2 started: v2 -> v3 (canary first)

$ ./bin/deployctl status
Stable version: v2
Latest deployment: Oct 31 15:10  #2   v2 -> v3     rolled back  1s   canary error rate 45% is above the 5% limit (9 of 20 requests failed)

PORT   PID     VERSION  STATE       REQUESTS   ERRORS  RESTARTS   UPTIME
9001   39613   v2       ready            349     0.0%         0      13s
9004   39608   v2       ready            565     0.0%         0      21s
9005   39612   v2       ready            373     0.0%         0      13s

Recent events
15:10:52  deployment #2 started: v2 -> v3
15:10:52  started v3 as canary on port 9002 (pid 39631)
15:10:52  v3 on port 9002 is ready
15:10:52  canary v3 is live on port 9002, watching its traffic for 8 s
15:10:53  stopped v3 on port 9002
15:10:53  deployment #2 failed: canary error rate 45% is above the 5% limit (9 of 20 requests failed). Rolling back to v2
15:10:53  deployment #2 rolled back: users are on v2
```

Meanwhile `loadgen`, playing the users, sent 4,293 requests across three
deployments (v1 → v2, v2 → v3 rolled back, back to v1). Only the 9 requests
the faulty canary answered failed, and **none** were unavailable:

```
Total: 4293 requests
  v1: 851 ok  v2: 3422 ok  v3: 11 ok, 9 failed  | unavailable: 0
```

## How it works

```
                 deployctl ──── deploy API (:8081, localhost only) ────┐
                                                                       ▼
 users / loadgen ──► :8080  ┌──────────────────── controller ──────────────────────┐
                            │  load balancer ─┬──► v1  :9001  ┐                    │
                            │  (thread pool)  ├──► v1  :9002  │ child processes    │
                            │                 ├──► v1  :9003  │ (fork + exec)      │
                            │                 └──► v2  :9004  ┘ ◄── canary         │
                            │  supervisor: waitpid() → spot crashes                  │
                            │  monitor:    GET /health every second, restart crashed │
                            │  deployer:   canary → rolling update → (rollback)      │
                            └────────────────────────────────────────────────────────┘
```

**A deployment, step by step** ([deployer.c](src/controller/deployer.c)):

1. **Canary.** Start one instance of the new version next to the old ones and wait for its health check.
   The load balancer gives it its share of traffic (1 of 4 instances ≈ 25%).
2. **Watch.** For `-w` seconds (default 20), it fails if it crashes, stops passing health checks,
   or fails more than `-e` percent (default 5) of at least 20 requests.
3. **Rolling update.** Start a new instance, wait until it is healthy, *then* gracefully stop an old one.
   Repeat. Capacity never drops below the target.
4. **Rollback** on any failure, or on `deployctl abort`: make sure enough old instances are running,
   then remove every instance of the new version. The result goes into the history file.

At the same time, the controller keeps instances healthy: crashed instances are
restarted with exponential backoff (1, 2, 4 … 30 s), instances that fail 3
health checks in a row stop getting traffic, and the load balancer retries a
request on another instance if the first one cannot be reached.

## Quick start

Needs a C compiler and `make` (Linux or macOS). Run everything from the project folder.

```bash
make

./bin/controller              # terminal 1: starts 3 instances of v1
./bin/loadgen                 # terminal 2: steady traffic, prints who answered
./bin/deployctl watch         # terminal 3: live view of instances and events

./bin/deployctl deploy v2     # terminal 4: a good release, rolls out
./bin/deployctl deploy v3     # a buggy release, rolled back automatically
./bin/deployctl deploy v4     # a release that crashes, rolled back
kill -9 <pid of an instance>  # watch it get restarted
./bin/deployctl history
```

Each version is a script in [`releases/`](releases/). The controller runs it with
the instance's port as its argument, so any program that serves HTTP on that port
and answers `GET /health` can be deployed.

| Release | Behaviour | What happens |
|---|---|---|
| `v1` | works | the starting version |
| `v2` | works | canary passes, rolls out to all instances |
| `v3` | 40% of requests return HTTP 500, but `/health` is fine | canary error rate too high → rollback |
| `v4` | crashes after 30 requests | canary crashes → rollback |
| `v5` | works, but takes 5 s to start | gets no traffic until its health check passes |

Controller options: `-n` instances (3), `-p` load balancer port (8080), `-a` API port (8081),
`-b` first instance port (9001), `-w` canary watch time, `-e` max error %, `-v` first version,
`-d` data folder. State, history, an event log and one log file per instance are kept in `data/`.

## The deploy API

`deployctl` is just a client for this JSON API, so you can call it with `curl` too:

| Method | Path | Body | Result |
|---|---|---|---|
| `GET` | `/status` | | instances, current deployment, recent events |
| `GET` | `/history` | | past deployments, newest first |
| `GET` | `/releases` | | versions that can be deployed |
| `POST` | `/deploy` | `{"version":"v2"}` | `202 Accepted`: runs in the background |
| `POST` | `/rollback` | | deploys the previous version again |
| `POST` | `/abort` | | stops the running deployment and rolls back |

```bash
curl -s -X POST localhost:8081/deploy -d '{"version":"v2"}'
# {"message":"deployment #1 started: v1 -> v2 (canary first)"}

curl -s -X POST localhost:8081/deploy -d '{"version":"v2"}'
# {"error":"deployment #1 (v1 -> v2) is still running"}        <- HTTP 409 Conflict
```

## Operating-system concepts used

| Concept | Where | Used for |
|---|---|---|
| `fork`, `execl`, `waitpid` | [instances.c](src/controller/instances.c) | start instances, collect exit status, avoid zombies |
| process groups (`setpgid`, `kill(-pgid)`) | instances.c | signal an instance and anything it started; keep Ctrl+C away from instances |
| `dup2`, closing inherited fds | instances.c | send each instance's output to its own log file; don't leak sockets into children |
| signals: `SIGTERM` → `SIGKILL` | instances.c | graceful stop with a time limit |
| `sigwait` with blocked signals | [main.c](src/controller/main.c) | handle Ctrl+C in one thread, no async signal handler |
| TCP sockets, `poll` | [net.c](src/common/net.c), [proxy.c](src/controller/proxy.c) | load balancer, health checks, API |
| thread pool + bounded queue | [thread_pool.c](src/controller/thread_pool.c) | producer–consumer with two condition variables |
| mutex + condition variable | instances.c, deployer.c | shared instance table; "sleep until the canary is ready" |
| atomic variables | deployer.c, main.c | shutdown and abort flags read inside locked loops |
| `fsync` + atomic `rename` | deployer.c | state file is never half-written, even on a crash |
| ring buffer | [events.c](src/controller/events.c) | the last 50 events, for `deployctl status` |

[docs/DESIGN.md](docs/DESIGN.md) explains the design decisions: the threads and locks, why
everything before `exec` must be async-signal-safe, how a graceful stop works, and more.

## Tests

```bash
make test   # 17 end-to-end checks against a real controller, about 20 s
make tsan   # the same checks with the controller built with ThreadSanitizer
```

[tests/e2e_test.sh](tests/e2e_test.sh) starts a controller on spare ports, sends
traffic with `loadgen`, and checks good, buggy and crashing releases, a `kill -9`'d
instance, refused requests, a restart that keeps the deployed version and history,
and that stopping the controller leaves no processes behind. The ThreadSanitizer
run reports any data race in the controller as a failure. Both run in CI on Linux and macOS.

Measured on an Apple M2 laptop: the load balancer forwarded **63,167 requests in
10 seconds (about 6,300 per second)** with no errors, opening a new TCP connection
for every request.

## Project layout

```
src/
  controller/        the controller
    main.c           startup, signals, shutdown order
    instances.c      starting/stopping processes, supervisor, health checks
    deployer.c       canary, rolling update, rollback, state and history
    proxy.c          load balancer
    thread_pool.c    worker threads for the load balancer
    api.c            deploy API for deployctl
    events.c         event log
  deployctl/         command-line client
  loadgen/           traffic generator
  demo-service/      the service being deployed
  common/            HTTP, JSON, sockets, growable strings (shared by all programs)
releases/            one script per version
tests/               end-to-end test
```

## Limits, and what a real system adds

This runs on one machine, which keeps every mechanism visible. Real systems go further:

- **Isolation.** Instances are plain processes. Containers add namespaces and cgroups for resource limits.
- **Many machines.** Kubernetes or Harness run an agent on each host that the control plane talks to.
- **Smarter verification.** The canary is judged on a fixed error-rate limit. Harness CD and similar
  tools also compare latency and error rates against the old version statistically.
- **Security.** The deploy API has no login, so it only listens on `127.0.0.1`.
- **HTTP.** Only a small subset of HTTP/1.1 is supported: one request per connection, no TLS.
