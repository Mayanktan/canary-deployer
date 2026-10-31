#ifndef THREAD_POOL_H
#define THREAD_POOL_H

// A fixed set of worker threads that serve client connections.
//
// The accepting thread puts each new connection into a bounded queue
// (producer); idle workers take connections out and handle them (consumers).
// When every worker is busy and the queue is full, pool_submit() waits, so a
// traffic spike waits in the kernel's listen backlog instead of making the
// controller start an unlimited number of threads.

typedef struct ThreadPool ThreadPool;

ThreadPool *pool_create(int workers, int queue_capacity, void (*handle)(int client_fd));
void        pool_submit(ThreadPool *pool, int client_fd);
void        pool_destroy(ThreadPool *pool);  // finishes queued connections, then joins the workers

#endif
