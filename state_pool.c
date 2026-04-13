#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>

#include "state_pool.h"

// Allocate and initialize memory pool for request states
req_state_pool * req_state_pool_alloc(uint16_t max_requests){
    req_state_pool *pool;

    if((pool = malloc(sizeof(req_state_pool))) == NULL || (pool->arr = calloc(max_requests, sizeof(request_state))) == NULL)
        return NULL;

    for(uint16_t i = 0; i < max_requests - 1; i++){
        for(uint8_t j = 0; j < MAX_RANGES; j++)
            pool->arr[i].ranges.arr[j].end = pool->arr[i].ranges.arr[j].start = -1;

        pool->arr[i].next = &pool->arr[i + 1];
    }
    pool->arr[max_requests - 1].next = NULL;
    pool->free_list_head = pool->arr;

    pthread_mutex_init(&pool->mutex, NULL);

    return pool;
}

// Acquire a request state struct from the pool
request_state * req_state_pool_acquire(req_state_pool *pool){
    pthread_mutex_lock(&pool->mutex);

    request_state *r = pool->free_list_head;
    if(r)
        pool->free_list_head = r->next;

    pthread_mutex_unlock(&pool->mutex);

    return r;
}

// Reset a request state struct to reuse it
void reset_state(request_state *r){
    r->req_buf.written = r->body.fsent = r->resp_header.sent = r->ranges.n_ranges = r->ranges.total_size = r->ranges.ranges_sent = 0;
    r->body.fd = -1;
    r->stage = READ;

    for(uint8_t i = 0; i < MAX_RANGES; i++)
        r->ranges.arr[i].end = r->ranges.arr[i].start = -1;
}

// Release an object and return it to the pool, after closing the associated socket and resetting state
void req_state_pool_release(req_state_pool *pool, request_state *r){
    close(r->sockfd);
    reset_state(r);

    pthread_mutex_lock(&pool->mutex);

    r->next = pool->free_list_head;
    pool->free_list_head = r;

    pthread_mutex_unlock(&pool->mutex);
}