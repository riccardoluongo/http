#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

#define MAX_HEADERS 64
#define N_MIME_TYPES 20
#define DEFAULT_HEADERS_LEN 512
#define ERR_RESPONSE_LEN 256
#define MAX_HEADER_VALUES 16
#define BACKLOG 32
#define ERR_400 0
#define ERR_500 1
#define ERR_404 2
#define ERR_304 3
#define GET_METHOD 0
#define HEAD_METHOD 1
#define CONN_QUEUE_LEN 1024
#define MAX_HEADER_VALUES 16
#define LOG_HEADER_LEN 48
#define LOG_MSG_BUF_LEN 256
#define TIME_BUF_SIZE 32

enum log_level{
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERR
};

typedef struct Header {
    char *key;
    char *values[MAX_HEADER_VALUES];
    void *next;

    uint8_t n_values; // Specifies the number of values the header has.
    uint8_t can_combine; // Specifies if the header can be combined in a comma or semicolon separated list. If set to 1, only the first or last value should be read.
} Header;

typedef struct{
    Header *arr;
    uint16_t capacity;
} ht;

typedef struct {
    const char *extension;
    const char *mime_type;
} MimeType;

typedef struct {
    int fds[CONN_QUEUE_LEN];

    int16_t front;
    int16_t rear;

    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} fd_queue;

struct date_enum{
    char *name;
    uint8_t num;
};

ht * ht_alloc(uint8_t max_headers);
void ht_free(ht *table);
Header * get_header(ht *table, char *key);
int8_t set_header(ht* table, char* header);
int8_t headercheck(const char *key);

fd_queue * fd_queue_alloc();
void fd_enqueue(int fd, fd_queue *queue);
int fd_dequeue(fd_queue *queue);