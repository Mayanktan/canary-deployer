#include "thread_pool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

struct ThreadPool {
    pthread_t *threads;
    int        thread_count;

    int *queue;     // circular buffer of client sockets waiting for a worker
    int  capacity;
    int  head;      // position of the oldest waiting socket
    int  count;     // how many sockets are waiting

    pthread_mutex_t lock;
    pthread_cond_t  not_empty;  // signalled when a socket is added
    pthread_cond_t  not_full;   // signalled when a socket is taken
    int             shutting_down;

    void (*handle)(int client_fd);
};

static void *worker_main(void *arg) {
    ThreadPool *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        while (pool->count == 0 && !pool->shutting_down) {
            pthread_cond_wait(&pool->not_empty, &pool->lock);
        }
        if (pool->count == 0) {  // shutting down and nothing left to do
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }
        int client_fd = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->lock);

        pool->handle(client_fd);  // the slow part runs without holding the lock
    }
}

ThreadPool *pool_create(int workers, int queue_capacity, void (*handle)(int client_fd)) {
    ThreadPool *pool = calloc(1, sizeof *pool);
    pool->threads = calloc((size_t)workers, sizeof *pool->threads);
    pool->queue = calloc((size_t)queue_capacity, sizeof *pool->queue);
    if (!pool->threads || !pool->queue) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }
    pool->thread_count = workers;
    pool->capacity = queue_capacity;
    pool->handle = handle;
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full, NULL);

    for (int i = 0; i < workers; i++) {
        pthread_create(&pool->threads[i], NULL, worker_main, pool);
    }
    return pool;
}

void pool_submit(ThreadPool *pool, int client_fd) {
    pthread_mutex_lock(&pool->lock);
    while (pool->count == pool->capacity) {
        pthread_cond_wait(&pool->not_full, &pool->lock);
    }
    pool->queue[(pool->head + pool->count) % pool->capacity] = client_fd;
    pool->count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
}

void pool_destroy(ThreadPool *pool) {
    pthread_mutex_lock(&pool->lock);
    pool->shutting_down = 1;
    pthread_cond_broadcast(&pool->not_empty);  // wake every idle worker so it can exit
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
    free(pool->threads);
    free(pool->queue);
    free(pool);
}
