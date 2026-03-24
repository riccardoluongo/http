#include <stdint.h>
#include <sys/types.h>

#define REQ_BUF_SIZE 4096
#define RESPONSE_BUF_LEN 256

enum http_code {
    OK,
    ERR_400,
    ERR_500,
    ERR_404,
    ERR_405,
    ERR_414,
    ERR_304
};

enum request_stage {
    READ,
    WRITE,
};

typedef struct request_state {
    struct request_state *next; // Next available request_state struct in the memory pool
    const char *method_str, *version_str, *mimetype; // The string suffixes are to avoid ambiguity with the enums for method and version
    ssize_t fsize, fsent; // File size, number of file bytes sent
    int connfd, file; // File descriptors
    uint16_t buf_written; // Number of bytes written to the recv buffer
    uint16_t resp_header_len, resp_header_sent; // Length and number of bytes sent of the response header
    uint8_t status_code : 3; // Status code of the response to send
    uint8_t stage : 1; // Request handling stage
    uint8_t close_conn : 1; // Specifies wether the connection should be closed after sending a response
    char buf[REQ_BUF_SIZE]; // recv buffer
    char response_header[RESPONSE_BUF_LEN]; // Response header buffer (request line + headers)
} request_state;

typedef struct {
    request_state *arr, *free_list_head;
    pthread_mutex_t mutex;
} req_state_pool;

req_state_pool * req_state_pool_alloc(uint16_t max_requests);
request_state * req_state_pool_acquire(req_state_pool *pool);
void req_state_pool_release(req_state_pool *pool, request_state *r);