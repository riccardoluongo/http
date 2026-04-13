#include <stdint.h>
#include <sys/types.h>

#define REQ_BUF_SIZE 4096
#define RESPONSE_BUF_LEN 256
#define MAX_RANGES 8
#define MULTIPART_HEADER_LEN 256

enum http_code {
    OK,
    ERR_400,
    ERR_500,
    ERR_404,
    ERR_405,
    ERR_414,
    ERR_304,
    PARTIAL_206,
};

enum request_stage {
    READ,
    WRITE,
};

// Structs that make up a request_state struct, made to clarify and group related entries together
typedef struct {
    ssize_t start, end;
} range;

typedef struct {
    char str[MULTIPART_HEADER_LEN];
    uint16_t written, sent;
} multipart_header_sized_str;

typedef struct {
    range arr[MAX_RANGES];
    ssize_t total_size; // Total amount of bytes to send
    void *multipart_pool; // A memory pool with fixed offsets allocated when needed
    uint8_t n_ranges, ranges_sent; // Will be 0 if no range header or ranges invalid
} range_arr;

typedef struct {
    char buf[REQ_BUF_SIZE]; // Request buffer
    uint16_t written; // Number of bytes written to the request buffer
} request_buf;

typedef struct {
    char buf[RESPONSE_BUF_LEN]; // Response header buffer (request line + headers)
    uint16_t buf_len, sent; // Length and number of bytes sent of the response header
} response_header;

typedef struct {
    int fd; // File descriptor of the file to send as body
    ssize_t fsize, fsent; // File size, number of file bytes sent
} response_body;

typedef struct request_state {
    struct request_state *next; // Next available request_state struct in the memory pool
    const char *method_str, *version_str, *mimetype; // The "str" suffixes are to avoid ambiguity with the enums for method and version
    range_arr ranges; // Fields related to range request handling
    int sockfd; // Socket associated with the request
    uint8_t status_code : 4; // Status code of the response to send
    uint8_t stage : 1; // Request handling stage
    uint8_t close_conn : 1; // Specifies wether the connection should be closed after sending a response
    response_body body;
    response_header resp_header; // TODO: maybe give this a more appropriate name
    request_buf req_buf;
} request_state;

typedef struct {
    request_state *arr, *free_list_head;
    pthread_mutex_t mutex;
} req_state_pool;

req_state_pool * req_state_pool_alloc(uint16_t max_requests);
request_state * req_state_pool_acquire(req_state_pool *pool);
void req_state_pool_release(req_state_pool *pool, request_state *r);
void reset_state(request_state *r);