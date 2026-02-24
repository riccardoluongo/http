#ifdef __STDC_NO_THREADS__
    #error Multithreading support is required to compile this program!
#endif

#include <ctype.h>
#include <iso646.h>
#include <linux/limits.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>

#include "types.h"

MimeType mime_types[N_MIME_TYPES] = {
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

uint8_t max_headers = MAX_HEADERS;
enum log_level log_level = LOG_DEBUG;
char *index_page = "/index.html";
fd_queue *conn_queue;
const struct open_how how = {
    .flags = O_RDONLY,
    .mode = 0,
    .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV
};
const char *log_level_str[] = {
    "DEBUG",
    "INFO",
    "WARNING",
    "ERROR"
};
struct worker_thread_cleanup{
    void *ptrs[2];
    int fds[2];
    ht *headers_table;

    uint8_t n_ptrs, n_fds;
};

int strcmp_bsearch_wrapper(const void *a, const void *b){
    return strcmp(a, ((MimeType *)b)->extension);
}

int date_enum_cmp(const void *a, const void *b){
    return strcmp(a, ((struct date_enum *)b)->name);
}

// Store the current GMT date and time in RFC 1123 into BUF
void get_1123_date(char *buf, uint8_t bufsize){
    time_t now = time(NULL);
    struct tm t;

    gmtime_r(&now, &t);
    strftime(buf, bufsize, "%a, %d %b %Y %H:%M:%S GMT", &t);
}

// Convert RFC 1123 time string into timestamp.
// Return timestamp on success, -1 on pattern matching failure and -2 on invalid date
time_t string_to_timestamp(char *s){
    static const struct date_enum months[12] = {
        {.name = "Apr", .num = 3},
        {.name = "Aug", .num = 7},
        {.name = "Dec", .num = 11},
        {.name = "Feb", .num = 1},
        {.name = "Jan", .num = 0},
        {.name = "Jul", .num = 6},
        {.name = "Jun", .num = 5},
        {.name = "Mar", .num = 2},
        {.name = "May", .num = 4},
        {.name = "Nov", .num = 10},
        {.name = "Oct", .num = 9},
        {.name = "Sep", .num = 8}
    };

    char monthname[4];
    int rv;
    const struct date_enum *bsearch_result;
    struct tm tm_struct;

    if((rv = sscanf(s, "%*3s, %d %3s %d %d:%d:%d %*s", &tm_struct.tm_mday, monthname, &tm_struct.tm_year, &tm_struct.tm_hour, &tm_struct.tm_min, &tm_struct.tm_sec)) < 6)
        return -1;

    if((bsearch_result = bsearch(monthname, months, 12, sizeof(struct date_enum), date_enum_cmp)) == NULL)
        return -2;

    tm_struct.tm_mon = bsearch_result->num;
    tm_struct.tm_year -= 1900;

    return timegm(&tm_struct);
}

char * get_log_date(char *buf, uint8_t bufsize){
    time_t now = time(NULL);
    struct tm t;

    localtime_r(&now, &t);
    strftime(buf, bufsize, "%d/%m/%Y %H:%M:%S", &t);

    return buf;
}

// Used to print internal log messages related to the server.
void print_server_log(char *format, uint8_t severity, FILE *output_file, int argc, ...){
    va_list arg_list;
    char log_msg[LOG_MSG_BUF_LEN], time_buf[TIME_BUF_SIZE];
    int rv;

    rv = snprintf(log_msg, LOG_HEADER_LEN, "[%s - %s] ", get_log_date(time_buf, TIME_BUF_SIZE), log_level_str[severity]);

    va_start(arg_list, argc);
    vsnprintf(log_msg + rv, LOG_MSG_BUF_LEN - rv, format, arg_list);
    va_end(arg_list);

    fputs(log_msg, output_file);
}

// Create a socket on PORT, get it ready for accept() and return its file descriptor or -1 on failure
int startsock(char *port, uint8_t backlog){
    int sockfd = socket(AF_INET, SOCK_STREAM, 0), optval = 1;
    struct sockaddr_in addr;

    if(sockfd == -1){
        print_server_log("%s: %s", LOG_ERR, stderr, 2, "could not create socket\n", strerror(errno));
        return -1;
    }

    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1){
        print_server_log("%s: %s", LOG_ERR, stderr, 2, "setsockopt failed:\n", strerror(errno));
        close(sockfd);
        return -1;
    }

    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(atoi(port));

    if(bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) != 0){
        print_server_log("%s: %s", LOG_ERR, stderr, 2, "could not bind to address\n", strerror(errno));
        return -1;
    }

    if((listen(sockfd, backlog)) != 0){
        print_server_log("%s: %s", LOG_ERR, stderr, 2, "could not listen on socket\n", strerror(errno));
        return -1;
    }

    return sockfd;
}

/*
Loop until all LEN bytes of BUF have been sent over SOCKET.
Returns the number of bytes left, so 0 in case of success.
*/
int16_t sendall(int sockfd, char *buf, uint16_t len){
    int16_t total = 0, left = len, rv;

    while(total < len){
        if ((rv = send(sockfd, buf+total, left, 0)) == -1)
            break;

        total += rv;
        left -= rv;
    }

    return left;
}

int sendall_file(int sockfd, int file, int len){
    int total = 0, left = len, rv;

    while(total < len){
        if ((rv = sendfile(sockfd, file, NULL, len)) == -1)
            break;

        total += rv;
        left -= rv;
    }

    return left;
}

/* Read from socket SOCKFD until the request line and headers have been received or LEN bytes have been read.  REQLINE_LEN will be set to the lenght of the request line, and HEADERS_LEN will be set to the length of the headers.  Return 0 on success, -1 on error or -2 on connection closed */
int8_t recv_request(int sockfd, char *buf, uint16_t len, uint16_t *reqline_len, uint16_t *headers_len){
    int16_t rv = 0, written = 0;
    char *headers_end = buf, *reqline_end = buf;

    do{
        if((rv = recv(sockfd, buf + written, len - written, 0)) == 0)
            return -2;
        else if(rv == -1)
            return -1;

        written += rv;
    } while((reqline_end = memmem(buf, written, "\r\n", 2)) == NULL);

    *reqline_len = reqline_end - buf;

    while((headers_end = memmem(buf, written, "\r\n\r\n", 4)) == NULL){
        if((rv = recv(sockfd, buf + written, len - written, 0)) == 0)
            return -2;
        else if(rv == -1)
            return -1;

        written += rv;
    }

    *headers_len = headers_end - reqline_end;

    return 0;
}

// Send http response through socket SOCKFD.
// Pass 0 to BODYFD or NULL to HEADERS if you do not wish to send any data for those fields.
// Arguments are to be passed with their ending CRLF sequences.
// Returns 0 on success, -1 on failure
int8_t send_response(int sockfd, char *req_line, char *headers, int bodyfd, uint16_t headers_len, size_t bodylen, uint16_t reqline_len, const char *mime_type){
    char default_headers[DEFAULT_HEADERS_LEN], time_buf[32];
    int16_t default_headers_len;

    if(sendall(sockfd, req_line, reqline_len) != 0)
        return -1;

    get_1123_date(time_buf, 32);
    default_headers_len = snprintf(default_headers, DEFAULT_HEADERS_LEN, "Date: %s\r\nContent-Length: %ld\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", time_buf, bodylen, mime_type);

    if(sendall(sockfd, default_headers, default_headers_len) != 0)
        return -1;

    if(headers != NULL && sendall(sockfd, headers, headers_len) != 0)
        return -1;

    if(bodyfd != 0 && sendall_file(sockfd, bodyfd, bodylen) != 0)
        return -1;

    return 0;
}

// Send an error response through SOCKFD depending on CODEFLAG.
// Return 0 on success, -1 on sending error and -2 on unrecognized CODEFLAG
int8_t send_error_response(int sockfd, uint8_t codeflag){
    char request[ERR_RESPONSE_LEN], time_buf[32];
    int16_t req_len;

    get_1123_date(time_buf, 32);

    switch(codeflag){
        case ERR_400:
            req_len = snprintf(request, ERR_RESPONSE_LEN, "HTTP/1.1 400 Bad Request\r\nDate: %s\r\nContent-Length: 24\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<h1>400 Bad Request</h1>", time_buf);
            break;
        case ERR_500:
            req_len = snprintf(request, ERR_RESPONSE_LEN, "HTTP/1.1 500 Internal Server Error\r\nDate: %s\r\nContent-Length: 36\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<h1>500 Internal Server Error</h1>", time_buf);
            break;
        case ERR_404:
            req_len = snprintf(request, ERR_RESPONSE_LEN, "HTTP/1.1 404 Not Found\r\nDate: %s\r\nContent-Length: 22\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<h1>404 Not Found</h1>", time_buf);
            break;
        case ERR_304:
            req_len = snprintf(request, ERR_RESPONSE_LEN, "HTTP/1.1 304 Not Modified\r\nDate: %s\r\nConnection: close\r\n\r\n", time_buf);
            break;
        default:
            return -2;
    }

    return sendall(sockfd, request, req_len) != 0 ? -1 : 0;
}

// Send redirect response through SOCKFD with target TARGET_URI.
// Return 0 on success, -1 on sending error and -2 if TARGET_URI would overflow the buffer
int8_t send_redirect_response(int sockfd, const char *target_uri){
    char request[ERR_RESPONSE_LEN], time_buf[32];
    int16_t req_len;

    get_1123_date(time_buf, 32);

    if((req_len = snprintf(request, ERR_RESPONSE_LEN, "HTTP/1.1 301 Moved Permanently\r\nDate: %s\r\nLocation: %s\r\nConnection: close\r\n\r\n", time_buf, target_uri)) > ERR_RESPONSE_LEN)
        return -2;

    return sendall(sockfd, request, req_len) != 0 ? -1 : 0;
}

// Send method not allowed response through SOCKFD
// Return 0 on success and -1 on failure
int8_t send_405_response(int sockfd){
    char request[ERR_RESPONSE_LEN], time_buf[32];
    int16_t req_len;
    int rv;

    get_1123_date(time_buf, 32);
    req_len = snprintf(request, ERR_RESPONSE_LEN, "HTTP/1.1 405 Method Not Allowed\r\nDate: %s\r\nAllow: GET, HEAD\r\nConnection: close\r\n\r\n", time_buf);

    return sendall(sockfd, request, req_len) != 0 ? -1 : 0;
}

// Parse the http request from buffer BUF and store its headers in HEADERS.
// The items in the request line will be turned into null terminated strings and the headers put into a hash table (which will first need to be allocated with ht_alloc); the body will remain unchanged.
// Return a number corresponding to the request's method on success or a negative value in case of error.
// Error codes:
//      -1: bad request
//      -2: internal error
//      -3: invalid method
int8_t parse_request(char *req_buf, uint16_t reqline_len, uint16_t headers_len, uint8_t max_headers, ht *headers_table){
    char *headers_ptr = req_buf + reqline_len + 2;
    const char *headers_end = headers_ptr + headers_len;
    int8_t method;

    // Replace the spaces in the request line to make us able to treat every piece as a string
    for(char i = 0, *separator = req_buf; i < 2; i++){ // Declaring i as char to be able to also declare the separator pointer here
        if((separator = memchr(separator, ' ', reqline_len - (separator - req_buf))) == NULL)
            return -1;

        *separator++ = '\0';

        if(isspace(*separator)) // Too many spaces
            return -1;
    }



    if(strcmp(req_buf, "GET") == 0)
        method = GET_METHOD;
    else if(strcmp(req_buf, "HEAD") == 0)
        method = HEAD_METHOD;
    else
        return -1;



    char *header_end; // Used in the loop below to point to the end of the current header
    while(headers_ptr < headers_end){
        if((header_end = memmem(headers_ptr, headers_end - headers_ptr, "\r\n", 2)) == NULL)
            return -1;
        *header_end = '\0';

        if(set_header(headers_table, headers_ptr) < 0)
            return -1;

        headers_ptr = header_end + 2;
    }

    return method;
}

void worker_cleanup(struct worker_thread_cleanup *struct_ptr){
    for(uint8_t i = 0; i < struct_ptr->n_ptrs; i++)
        free(struct_ptr->ptrs[i]);

    for(uint8_t i = 0; i < struct_ptr->n_fds; i++)
        if(struct_ptr->fds[i] != -1) close(struct_ptr->fds[i]);

    memset(struct_ptr->headers_table->arr, 0, struct_ptr->headers_table->capacity * sizeof(Header));
}

void * handle_client(void *arg){
    int file, sockfd, pagesize = *(int*)arg;
    char *req_buf = malloc(pagesize), *path, *extension; // TODO malloc err handling
    char client_addr[INET_ADDRSTRLEN];
    const char *mimetype;
    int8_t rv, request_method;
    uint16_t reqline_len = 0, headers_len = 0;
    ht *headers_table;
    struct stat stat_buf;
    struct sockaddr addr;
    socklen_t addrlen = sizeof(addr);

    struct worker_thread_cleanup cleanup_struct = {
        .fds[0] = -1,
        .fds[1] = -1,
        .headers_table = NULL,

        .n_ptrs = 0,
        .n_fds = 2
    };

    if((headers_table = ht_alloc(max_headers)) == NULL){
        print_server_log("could not allocate headers table: %s\n", LOG_ERR, stderr, 1, strerror(errno));

        worker_cleanup(&cleanup_struct);
        pthread_exit(NULL); // TODO err handling in main
    }

    cleanup_struct.headers_table = headers_table;

    while(1){
        cleanup_struct.fds[0] = sockfd = fd_dequeue(conn_queue);

        if(getpeername(sockfd, &addr, &addrlen) == -1){
            print_server_log("worker could not get client addr: %s\n", LOG_ERR, stderr, 1, strerror(errno));
            worker_cleanup(&cleanup_struct);
            continue;
        }

        inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, client_addr, addrlen);
        print_server_log("handling client: %s\n", LOG_INFO, stdout, 1, client_addr);

        if((rv = recv_request(sockfd, req_buf, pagesize, &reqline_len, &headers_len)) == -1){
            print_server_log("recv: %s", LOG_ERR, stderr, 1, strerror(errno));

            worker_cleanup(&cleanup_struct);
            continue;
        } else if(rv == -2){
            print_server_log("client closed connection\n", LOG_ERR, stderr, 0);

            worker_cleanup(&cleanup_struct);
            continue;
        }


        request_method = parse_request(req_buf, reqline_len, headers_len, max_headers, headers_table);
        if(request_method < 0){
            switch(request_method){
                case -1:
                    print_server_log("bad request\n", LOG_WARN, stdout, 0);
                    rv = send_error_response(sockfd, ERR_400);
                    break;
                case -2:
                    print_server_log("internal error\n", LOG_ERR, stderr, 0);
                    rv = send_error_response(sockfd, ERR_500);
                    break;
                case -3:
                    print_server_log("invalid method %s\n", LOG_WARN, stdout, 1, req_buf);
                    rv = send_405_response(sockfd);
                    break;
                }

            if(rv < 0)
                print_server_log("could not send error response\n", LOG_ERR, stderr, 0);

            worker_cleanup(&cleanup_struct);
            continue;
        }

        path = req_buf + (request_method ? 5 : 4);

        if(strlen(path) > 128){
            print_server_log("path too long", LOG_ERR, stderr, 0);

            if((rv = send_error_response(sockfd, ERR_400)) == -1)
                print_server_log("could not send error response: %s\n", LOG_ERR, stderr, 1, strerror(errno));
            else if(rv == -2)
                print_server_log("could not send error response: invalide CODEFLAG\n", LOG_ERR, stderr, 0);

            worker_cleanup(&cleanup_struct);
            continue;
        }

        if(path[0] == '/' && path[1] == '\0')
            path = index_page;

        if((cleanup_struct.fds[1] = file = syscall(__NR_openat2, AT_FDCWD, path, &how, sizeof(how))) == -1){
            print_server_log("could not open file: %s", LOG_ERR, stderr, 1, strerror(errno));

            rv = errno == ENOENT ? send_error_response(sockfd, ERR_404) : send_error_response(sockfd, ERR_500);

            if(rv == -1)
                print_server_log("could not send error response: %s\n", LOG_ERR, stderr, 1, strerror(errno));
            else if(rv == -2)
                print_server_log("could not send error response: invalide CODEFLAG\n", LOG_ERR, stderr, 0);

            worker_cleanup(&cleanup_struct);
            continue;
        }

        if(fstat(file, &stat_buf) == -1){
            print_server_log("could not read file length: %s\n", LOG_ERR, stderr, 1, strerror(errno));

            if((rv = send_error_response(sockfd, ERR_500) == -1))
                print_server_log("could not send error response: %s\n", LOG_ERR, stderr, 1, strerror(errno));
            else if(rv == -2)
                print_server_log("could not send error response: invalid CODEFLAG\n", LOG_ERR, stderr, 0);

            worker_cleanup(&cleanup_struct);
            continue;
        }

        Header *if_modified_since = get_header(headers_table, "if-modified-since");
        MimeType *bsearch_rv;

        if(if_modified_since != NULL){
            time_t header_timestamp = string_to_timestamp(if_modified_since->values[0]);

            switch(header_timestamp){
                case -1:
                    print_server_log("could not calculate if-modified-since timestamp: pattern matching failure\n", LOG_ERR, stderr, 0);
                    worker_cleanup(&cleanup_struct);
                    continue;
                case -2:
                    print_server_log("could not calculate if-modified-since timestamp: invalid date\n", LOG_ERR, stderr, 0);
                    worker_cleanup(&cleanup_struct);
                    continue;
            }

            if(header_timestamp >= stat_buf.st_mtime){
                if((rv = send_error_response(sockfd, ERR_304)) < 0)
                    print_server_log("could not send error response: %s\n", LOG_ERR, stderr, 1, strerror(errno));

                worker_cleanup(&cleanup_struct);
                continue;
            }
        }

        if((extension = strrchr(path, '.')) != NULL && (bsearch_rv = bsearch(extension, mime_types, N_MIME_TYPES, sizeof(MimeType), strcmp_bsearch_wrapper)) != NULL)
            mimetype = bsearch_rv->mime_type;
        else
            mimetype = "application/octet-stream";

        if(send_response(sockfd, "HTTP/1.1 200 OK\r\n", NULL, request_method ? 0 : file, 0, stat_buf.st_size, 17, mimetype) == -1)
            print_server_log("could not send response: %s\n", LOG_ERR, stderr, 1, strerror(errno));

        worker_cleanup(&cleanup_struct);
    }

    pthread_exit(NULL);
}

int main(int argc, char ** argv){
    char *port = "8080", client_addr[INET_ADDRSTRLEN];
    int listening_socket, incoming_socket, pagesize = getpagesize();
    struct sockaddr_storage addr;
    socklen_t addr_size = sizeof(addr);
    int16_t max_headers_input;
    pthread_t tid;
    uint8_t backlog = 20, tnum = sysconf(_SC_NPROCESSORS_ONLN);

    for(int i = 1; i < argc && argv[i][0] == '-'; i+=2)
        switch(argv[i][1]){
            case 'h':
                printf("simple http server written in pure C\nusage: server [options]\n\toptions:\n\t\t-p\tPort to bind to (default: 8080)\n\t\t-m\tMaximum amount of headers accepted (default: 64)\n");
                return 0;
            case 'p':
                if(strlen((port = argv[i+1])) > 5 || atoi(port) > 65535){
                    fprintf(stderr, "error: invalid port number provided\n");
                    return -1;
                }
                else
                    break;
            case 'm':
                max_headers_input = atoi(argv[i+1]); // TODO check here too
                max_headers = max_headers_input > 0 ? max_headers_input : MAX_HEADERS;
                break;
            case 'b':
                if((backlog = atoi(argv[i+1])) > 254 || backlog < 1){
                    fprintf(stderr, "invalid backlog number. Choose one between 1 and 254");
                    exit(-1);
                }
                break;
            case 't':
                if((tnum = atoi(argv[i+1])) > 254 || tnum < 1){
                    fprintf(stderr, "invalid thread count. Choose a number between 1 and 254");
                    exit(-1);
                }
        }

    if((listening_socket = startsock(port, backlog)) == -1){
        print_server_log("startsock failed: %s\n", LOG_ERR, stderr, 1, strerror(errno));
        exit(-1);
    }

    if((conn_queue = fd_queue_alloc()) == NULL){
        print_server_log("could not allocate queue: %s\n", LOG_ERR, stderr, 1, strerror(errno));
        exit(-1);
    }

    for(uint8_t i = 0; i < tnum; i++){
        pthread_create(&tid, NULL, handle_client, &pagesize);
        pthread_detach(tid);
    }

    print_server_log("server started on port %s\nmax. request headers: %d\nrequest buffer size: %d (same as system page size)\nnumber of threads: %d (same as number of cores)\n", LOG_INFO, stdout, 4, port, max_headers, pagesize, tnum);

    while(1){
        if((incoming_socket = accept(listening_socket, (struct sockaddr *)&addr, &addr_size)) == -1){
            print_server_log("could not accept connection: %s\n", LOG_ERR, stderr, 1, strerror(errno));
            exit(-1);
        }

        fd_enqueue(incoming_socket, conn_queue);
    }
}