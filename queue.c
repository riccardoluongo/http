#include "types.h"
#include <stdlib.h>

fd_queue * fd_queue_alloc(){
    fd_queue *new_queue;

    if((new_queue = malloc(sizeof(fd_queue))) == NULL)
        return NULL;

    new_queue->front = new_queue->rear = -1;
    pthread_cond_init(&new_queue->not_empty, NULL);
    pthread_cond_init(&new_queue->not_full, NULL);
    pthread_mutex_init(&new_queue->mutex, NULL);

    return new_queue;
}

void fd_enqueue(int fd, fd_queue *queue){
    pthread_mutex_lock(&queue->mutex);

    while((queue->front == (queue->rear + 1) % CONN_QUEUE_LEN) || (queue->front == 0 && queue->rear == CONN_QUEUE_LEN - 1))
        pthread_cond_wait(&queue->not_full, &queue->mutex);

    if(queue->front == -1)
        queue->front = 0;

    queue->fds[(queue->rear = (queue->rear + 1) % CONN_QUEUE_LEN)] = fd;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

int fd_dequeue(fd_queue *queue){
    pthread_mutex_lock(&queue->mutex);

    while(queue->front == -1)
        pthread_cond_wait(&queue->not_empty, &queue->mutex);

    int element = queue->fds[queue->front];
    if(queue->front == queue->rear)
        queue->front = queue->rear = -1;
    else
        queue->front = (queue->front + 1) % CONN_QUEUE_LEN;

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);

    return element;
}