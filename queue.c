#include "queue.h"
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

/**
arr size = queue slots + 1 empty slot = 8 slots
q  :  [ 1 2 3 4 5 6 7 ]
arr:  [ 6 7 _ 1 2 3 4 5]
ptr:        t h
(h)ead is for popping, always points to a valid element (unless empty)
(t)ail is for pushing, always points to an invalid element
queue is empty when head == tail
queue is full when head === tail+1 mod arr_size 

WHY the empty slot?
	let L be the array size
	(#LENGTHS) * (#STARTS) = (L+1)L possible queue states
	head and tail can only represent L*L states
	so we need an extra array slot (or extra logic)

we could instead use a 'length' field instead of tail,
since 'length' would have n+1 possible values
**/
struct queue {
    // queue stuff
    int arr_size;
    int head;
    int tail;
    void **arr;
    // thread stuff
    pthread_mutex_t lock;
    pthread_cond_t allow_push;
    pthread_cond_t allow_pop;
};

queue_t *queue_new(int size) {
    queue_t *pq = malloc(sizeof(queue_t));
    pq->arr_size = size + 1; // +1 empty slot
    pq->head = 0;
    pq->tail = 0;
    pq->arr = calloc(sizeof(void *), pq->arr_size);

    // thread stuff
    int err = pthread_mutex_init(&(pq->lock), NULL);
    err |= pthread_cond_init(&(pq->allow_push), NULL);
    err |= pthread_cond_init(&(pq->allow_pop), NULL);
    if (err) {
        perror("pthread init error");
        exit(1);
    }

    return pq;
}

void queue_delete(queue_t **q) {
    pthread_mutex_destroy(&((*q)->lock));
    pthread_cond_destroy(&((*q)->allow_push));
    pthread_cond_destroy(&((*q)->allow_pop));

    free((*q)->arr);
    free(*q);
    *q = NULL;
}

bool queue_push(queue_t *q, void *elem) {
    if (q == NULL)
        return false;

    pthread_mutex_lock(&(q->lock));
    while (((q->tail + 1) % q->arr_size) == q->head) { // Q FULL, BLOCK
        pthread_cond_wait(&(q->allow_push), &(q->lock));
    }

    q->arr[q->tail] = elem;
    q->tail = (q->tail + 1) % q->arr_size;

    pthread_cond_signal(&(q->allow_pop)); // guarentee Q is nonempty
    pthread_mutex_unlock(&(q->lock));
    return true;
}

bool queue_pop(queue_t *q, void **elem) {
    if (q == NULL)
        return false;

    pthread_mutex_lock(&(q->lock));
    while (q->head == q->tail) { // Q EMPTY, BLOCK
        pthread_cond_wait(&(q->allow_pop), &(q->lock));
    }

    *elem = q->arr[q->head];
    q->head = (q->head + 1) % q->arr_size;

    pthread_cond_signal(&(q->allow_push)); // guarentee Q is not full
    pthread_mutex_unlock(&(q->lock));
    return true;
}
