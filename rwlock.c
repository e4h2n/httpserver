#include "rwlock.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

struct rwlock {
    PRIORITY priority;
    int max_wait;
    int countdown; // for n-way
    int waiting_readers;
    int active_readers;
    int waiting_writers;
    int active_writers;

    pthread_mutex_t lock; // for internal atomicity
    pthread_cond_t read_go;
    pthread_cond_t write_go;
};

rwlock_t *rwlock_new(PRIORITY p, uint32_t n) {
    rwlock_t *rw = malloc(sizeof(rwlock_t));
    rw->priority = p;
    rw->max_wait = n;
    rw->countdown = 0;
    rw->waiting_readers = 0;
    rw->active_readers = 0;
    rw->waiting_writers = 0;
    rw->active_writers = 0;
    int err = pthread_mutex_init(&(rw->lock), NULL);
    err |= pthread_cond_init(&(rw->read_go), NULL);
    err |= pthread_cond_init(&(rw->write_go), NULL);
    if (err) {
        perror("pthread init error");
        exit(1);
    }

    return rw;
}

void rwlock_delete(rwlock_t **rw) {
    pthread_mutex_destroy(&((*rw))->lock);
    pthread_cond_destroy(&((*rw))->read_go);
    pthread_cond_destroy(&((*rw))->write_go);

    free(*rw);
    *rw = NULL;
}

bool read_check(rwlock_t *rw) {
    return rw->active_writers > 0
           || (rw->waiting_writers > 0
               && (rw->priority == WRITERS || (rw->priority == N_WAY && rw->countdown <= 0)));
}

void reader_lock(rwlock_t *rw) {
    pthread_mutex_lock(&(rw->lock));

    rw->waiting_readers++; // in reading queue now
    while (read_check(rw)) {
        pthread_cond_wait(&(rw->read_go), &(rw->lock));
    }

    if (rw->waiting_writers > 0)
        rw->countdown--; // only decrement if countdown is "valid"
    rw->waiting_readers--; // done waiting
    rw->active_readers++;

    pthread_mutex_unlock(&(rw->lock));
}

void reader_unlock(rwlock_t *rw) {
    pthread_mutex_lock(&(rw->lock));

    rw->active_readers--;
    pthread_cond_signal(&(rw->write_go));
    pthread_mutex_unlock(&(rw->lock));
}

bool write_check(rwlock_t *rw) {
    return rw->active_readers > 0 || rw->active_writers > 0
           || (rw->waiting_readers > 0
               && (rw->priority == READERS || (rw->priority == N_WAY && rw->countdown > 0)));
}

void writer_lock(rwlock_t *rw) {
    pthread_mutex_lock(&(rw->lock));

    rw->waiting_writers++; // in writing queue now
    while (write_check(rw)) { // wait
        pthread_cond_wait(&(rw->write_go), &(rw->lock));
    }

    rw->countdown = rw->max_wait; // set countdown for next writer...I guess?

    rw->waiting_writers--;
    rw->active_writers++;
    pthread_mutex_unlock(&(rw->lock));
}

void writer_unlock(rwlock_t *rw) {
    pthread_mutex_lock(&(rw->lock));

    rw->active_writers--;
    pthread_cond_signal(&(rw->write_go));
    pthread_cond_broadcast(&(rw->read_go));
    pthread_mutex_unlock(&(rw->lock));
}
