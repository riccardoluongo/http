#ifdef __STDC_NO_THREADS__
    #error Multithreading support is required to compile this program!
#endif

#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <time.h>
#include <errno.h>
#include <sys/epoll.h>
#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <libgen.h>

#include "state_pool.h"
#include "uuidv4.h"

#define N_MIME_TYPES 20
#define MAX_HEADER_VALUES 16
#define BACKLOG 128
#define LOG_HEADER_LEN 48
#define LOG_MSG_BUF_LEN 256
#define TIME_BUF_SIZE 40
#define MAX_EVENTS 512
#define MAX_PATH_LEN 256
#define UUID_LEN 40 // 3 bytes larger to allow for pointer alignment in memory pool

// Rearm socket for write or exit on failure
#define WRITE_REARM_OR_DIE \
    do{ \
        if(epoll_ctl(epollfd, EPOLL_CTL_MOD, request->sockfd, &write_ev) == -1){ \
            print_log("could not rearm socket: %s, exiting.\n", LOG_ERR, stderr, strerror(errno)); \
            exit(-1); \
        } \
    } while(0)

// Write error handling
#define WRITE_ERR \
    if(errno != EPIPE) \
        exit(-1); \
    req_state_pool_release(pool, request); \
    return -1; \

enum log_level {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERR
};

enum method {
    GET,
    HEAD
};

enum http_ver {
    HTTP_1_1,
    HTTP_1_0,
};

typedef struct {
    const char *extension;
    const char *mime_type;
} mime_type;

typedef struct {
    char *name;
    uint8_t num;
} date_enum;

typedef struct {
    struct epoll_event *events;
    req_state_pool *pool;
    int epollfd, listenfd, root_fd;
} thread_args;

static mime_type mime_types[N_MIME_TYPES] = {
    { ".css", "text/css" },
    { ".flac", "audio/flac" },
    { ".gif", "image/gif" },
    { ".htm", "text/html" },
    { ".html", "text/html" },
    { ".ico", "image/x-icon" },
    { ".jpeg", "image/jpeg" },
    { ".jpg", "image/jpeg" },
    { ".js", "application/javascript" },
    { ".json", "application/json" },
    { ".mkv", "video/x-matroska" },
    { ".mp3", "audio/mp3" },
    { ".mp4", "video/mp4" },
    { ".pdf", "application/pdf" },
    { ".png", "image/png" },
    { ".srt", "application/x-subrip" },
    { ".svg", "image/svg+xml" },
    { ".txt", "text/plain" },
    { ".vtt", "text/vtt" },
    { ".xml", "application/xml" }
};

static const char *log_level_str[] = {
    "DEBUG",
    "INFO",
    "WARNING",
    "ERROR"
};

static const uint32_t epoll_write_flags = EPOLLOUT | EPOLLET | EPOLLONESHOT;
static const uint32_t epoll_read_flags = EPOLLIN | EPOLLET | EPOLLONESHOT;