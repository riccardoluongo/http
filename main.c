#include "main.h"
#include <unistd.h>

static uint8_t log_level = LOG_INFO;
static char *index_page = "/index.html";

int strcmp_bsearch_wrapper(const void *a, const void *b){
    return strcmp(a, ((mime_type *)b)->extension);
}

int date_enum_cmp(const void *a, const void *b){
    return strcmp(a, ((date_enum *)b)->name);
}

// Store the current GMT date and time in RFC 1123 into BUF
void get_1123_date(char *buf, uint8_t bufsize){
    struct tm t;
    time_t now = time(NULL);

    gmtime_r(&now, &t);
    strftime(buf, bufsize, "%a, %d %b %Y %H:%M:%S GMT", &t);
}

void print_log(char *format, uint8_t severity, FILE *output_file, ...){
    if(severity < log_level)
        return;

    struct tm t;
    struct timespec ts;
    time_t now = time(NULL);
    char log_msg[LOG_MSG_BUF_LEN], time_buf[TIME_BUF_SIZE];

    localtime_r(&now, &t);
    clock_gettime(CLOCK_REALTIME, &ts);
    sprintf(time_buf + strftime(time_buf, TIME_BUF_SIZE, "%d/%m/%Y %H:%M:%S.", &t), "%03ld", ts.tv_nsec / 1000000);

    int rv = snprintf(log_msg, LOG_HEADER_LEN, "[%s - %s] ", time_buf, log_level_str[severity]);

    va_list arg_list;

    va_start(arg_list, output_file);
    vsnprintf(log_msg + rv, LOG_MSG_BUF_LEN - rv, format, arg_list);
    va_end(arg_list);

    fputs(log_msg, output_file);
}

// Convert RFC 1123 time string into timestamp.
// Return timestamp on success or -1 on failure
time_t string_to_timestamp(char *s){
    static const date_enum months[12] = {
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

    struct tm header_tm_struct;
    char monthname[4], tz[4];

    // Try to parse 3 date formats
    // This code is very ugly but for now I have better things to think about
    if(sscanf(s, "%*3s, %d %3s %d %d:%d:%d %3s", &header_tm_struct.tm_mday, monthname, &header_tm_struct.tm_year, &header_tm_struct.tm_hour, &header_tm_struct.tm_min, &header_tm_struct.tm_sec, tz) == 7){
        if(strcmp(tz, "GMT") != 0) return -1;
    } else if(sscanf(s, "%*[^,], %d-%3s-%d %d:%d:%d %s", &header_tm_struct.tm_mday, monthname, &header_tm_struct.tm_year, &header_tm_struct.tm_hour, &header_tm_struct.tm_min, &header_tm_struct.tm_sec, tz) == 7){
        if(strcmp(tz, "GMT") != 0)
            return -1;

        struct tm current_time;
        time_t now = time(NULL);

        localtime_r(&now, &current_time);

        // Handle century ambiguity
        header_tm_struct.tm_year += (header_tm_struct.tm_year + 100) - current_time.tm_year < 50 ? 2000 : 1900;
    } else if(sscanf(s, "%*3s %3s %d %d:%d:%d %d", monthname, &header_tm_struct.tm_mday, &header_tm_struct.tm_hour, &header_tm_struct.tm_min, &header_tm_struct.tm_sec, &header_tm_struct.tm_year) == 6)
        ;
    else
        return -1;

    const date_enum *bsearch_result;
    if((bsearch_result = bsearch(monthname, months, 12, sizeof(date_enum), date_enum_cmp)) == NULL)
        return -1;

    header_tm_struct.tm_mon = bsearch_result->num;
    header_tm_struct.tm_year -= 1900;

    return timegm(&header_tm_struct);
}

// Create a socket on PORT, get it ready for accept() and return its file descriptor or -1 on failure
int startsock(uint16_t port, uint8_t backlog){
    struct sockaddr_in addr;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0), optval = 1;

    if(sockfd == -1){
        print_log("%s: %s", LOG_ERR, stderr, "could not create socket\n", strerror(errno));
        return -1;
    }

    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1){
        print_log("%s: %s", LOG_ERR, stderr, "setsockopt failed:\n", strerror(errno));
        close(sockfd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if(bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) != 0){
        print_log("%s: %s", LOG_ERR, stderr, "could not bind to address\n", strerror(errno));
        return -1;
    }

    if((listen(sockfd, backlog)) != 0){
        print_log("%s: %s", LOG_ERR, stderr, "could not listen on socket\n", strerror(errno));
        return -1;
    }

    return sockfd;
}

// Set socket to non blocking mode and return 0 on success or -1 on failure
int8_t setnonblocking(int sockfd){
    int sock_flags;
    if((sock_flags = fcntl(sockfd, F_GETFL, 0)) == -1){
        print_log("could not get socket flags: %s\n", LOG_ERR, stderr, strerror(errno));
        return -1;
    }

    if(fcntl(sockfd, F_SETFL, sock_flags | O_NONBLOCK) == -1){
        print_log("could not set socket flags: %s\n", LOG_ERR, stderr, strerror(errno));
        return -1;
    }

    return 0;
}

// Send response to REQUEST, which is part of epoll instance EPOLLFD.
// The STATUS_CODE argument will be ignored after the first function call (request is already sending)
// Return 0 when done, -1 on partial write, -2 on error and -3 on invalid status code
int8_t send_response(int epollfd, request_state *request, req_state_pool *pool, uint8_t status_code){
    if(request->resp_header_sent == 0){ // Nothing sent yet, prepare response
        char time_buf[TIME_BUF_SIZE];
        get_1123_date(time_buf, TIME_BUF_SIZE);

        request->status_code = status_code;

        switch(request->status_code){
            case ERR_400:
                request->resp_header_len = snprintf(request->response_header, RESPONSE_BUF_LEN, "HTTP/1.1 400 Bad Request\r\nDate: %s\r\nContent-Length: 24\r\nContent-Type: text/html%s\r\n\r\n<h1>400 Bad Request</h1>", time_buf, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case ERR_500:
                request->resp_header_len = snprintf(request->response_header, RESPONSE_BUF_LEN, "HTTP/1.1 500 Internal Server Error\r\nDate: %s\r\nContent-Length: 34\r\nContent-Type: text/html%s\r\n\r\n<h1>500 Internal Server Error</h1>", time_buf, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case ERR_404:
                request->resp_header_len = snprintf(request->response_header, RESPONSE_BUF_LEN, "HTTP/1.1 404 Not Found\r\nDate: %s\r\nContent-Length: 22\r\nContent-Type: text/html%s\r\n\r\n<h1>404 Not Found</h1>", time_buf, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case ERR_405:
                request->resp_header_len  = snprintf(request->response_header, RESPONSE_BUF_LEN, "HTTP/1.1 405 Method Not Allowed\r\nDate: %s\r\nAllow: GET, HEAD%s\r\n\r\n", time_buf, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case ERR_414:
                request->resp_header_len  = snprintf(request->response_header, RESPONSE_BUF_LEN, "HTTP/1.1 414 URI Too Long\r\nDate: %s\r\nContent-Length: 25\r\nContent-Type: text/html%s\r\n\r\n<h1>414 URI Too Long</h1>", time_buf, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case ERR_304:
                request->resp_header_len = snprintf(request->response_header, RESPONSE_BUF_LEN, "HTTP/1.1 304 Not Modified\r\nDate: %s%s\r\n\r\n", time_buf, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case OK:
                request->resp_header_len = snprintf(request->response_header, RESPONSE_BUF_LEN, "HTTP/1.1 200 OK\r\nDate: %s\r\nContent-Length: %ld\r\nContent-Type: %s%s\r\n\r\n", time_buf, request->fsize, request->mimetype, request->close_conn ? "\r\nConnection: close" : "");
                break;
            default:
                return -3;
        }

        if(request->status_code != 0)
            request->file = -1;
        request->stage = WRITE;
    }

    struct epoll_event write_ev = {epoll_write_flags, request}; // Used to rearm sockets for writes
    struct epoll_event read_ev = {epoll_read_flags, request}; // Used to rearm sockets for reads
    ssize_t sent;

    if(request->resp_header_sent < request->resp_header_len){
        if((sent = send(request->connfd, request->response_header, request->resp_header_len - request->resp_header_sent, request->file > 0 ? MSG_MORE : 0)) == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                if(epoll_ctl(epollfd, EPOLL_CTL_MOD, request->connfd, &write_ev) == -1){
                    print_log("could not rearm socket: %s\n", LOG_ERR, stderr, strerror(errno));
                    return -2;
                }

                return -1;
            } else{
                print_log("could not send response header: %s\n", LOG_ERR, stderr, strerror(errno));
                return -2;
            }
        } else if(sent == 0){
            req_state_pool_release(pool, request);
            return -2;
        }

        request->resp_header_sent += sent;

        if(request->resp_header_sent < request->resp_header_len){
            if(epoll_ctl(epollfd, EPOLL_CTL_MOD, request->connfd, &write_ev) == -1){
                print_log("could not rearm socket: %s\n", LOG_ERR, stderr, strerror(errno));
                return -2;
            }

            return -1;
        }
    }

    if(request->file > 0){ // If the response has a body, send it
        if((sent = sendfile(request->connfd, request->file, &request->fsent, request->fsize - request->fsent)) == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                if(epoll_ctl(epollfd, EPOLL_CTL_MOD, request->connfd, &write_ev) == -1){
                    print_log("could not rearm socket: %s\n", LOG_ERR, stderr, strerror(errno));
                    return -2;
                }

                return -1;
            } else{
                print_log("could not send response body: %s\n", LOG_ERR, stderr, strerror(errno));
                return -2;
            }
        } else if(sent == 0){
            req_state_pool_release(pool, request);
            return -2;
        }

        if(request->fsent < request->fsize){
            if(epoll_ctl(epollfd, EPOLL_CTL_MOD, request->connfd, &write_ev) == -1){
                print_log("could not rearm socket: %s\n", LOG_ERR, stderr, strerror(errno));
                return -2;
            }

            return -1;
        } else {
            close(request->file);
        }
    }

    // Reset request state
    request->buf_written = request->fsent = request->resp_header_sent = 0;
    request->file = -1;
    request->stage = READ;

    if(!request->close_conn){
        if(epoll_ctl(epollfd, EPOLL_CTL_MOD, request->connfd, &read_ev) == -1){
            print_log("could not rearm socket: %s\n", LOG_ERR, stderr, strerror(errno));
            return -2;
        }
    } else{
        close(request->connfd);
    }

    return 0;
}

// Main server logic executed by workers
void * handle_client(void *arg){
    thread_args * args = (thread_args *)arg; // Thread arguments struct passed via ARG
    int16_t rv; // Used to temporarily store return values when needed
    struct epoll_event ev = {.events = epoll_read_flags}; // Default epoll event struct

    while(1){
        int16_t fds_n;

        if((fds_n = epoll_wait(args->epollfd, args->events, MAX_EVENTS, -1)) == -1){
            print_log("epoll_wait failed: %s\n", LOG_ERR, stderr, strerror(errno));

            if(errno == EINTR)
                continue;
            exit(-1);
        }

        for(uint16_t i = 0; i < fds_n; i++){
            request_state *req_state = (request_state *)args->events[i].data.ptr;

            if(req_state->connfd == args->listenfd){ // New connection
                struct sockaddr addr;
                socklen_t addrlen = sizeof(addr);

                while(1){
                    // Accept connection
                    int sockfd;
                    if((sockfd = accept(req_state->connfd, &addr, &addrlen)) == -1){
                        if(errno == EAGAIN || errno == EWOULDBLOCK)
                            break;

                        print_log("could not accept connection: %s\n", LOG_ERR, stderr, strerror(errno));
                        continue;
                    }

                    // Make socket nonblocking
                    if(setnonblocking(sockfd) == -1)
                        continue;

                    // Add socket to interest list
                    ev.data.ptr = req_state_pool_acquire(args->pool);
                    ((request_state *)ev.data.ptr)->connfd = sockfd;
                    if(epoll_ctl(args->epollfd, EPOLL_CTL_ADD, sockfd, &ev) == -1){
                        print_log("could not add socket to interest list: %s\n", LOG_ERR, stderr, strerror(errno));
                        continue;
                    }

                    // Get client addr and print it
                    if(getpeername(sockfd, &addr, &addrlen) == -1){
                        print_log("could not get client addr: %s\n", LOG_ERR, stderr, strerror(errno));
                        continue;
                    }

                    char client_addr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, client_addr, addrlen);
                    print_log("got connection from: %s\n", LOG_INFO, stdout, client_addr);
                }
           } else { // Handle request
                if(req_state->stage == READ){
                    uint16_t buffer_avail = REQ_BUF_SIZE - req_state->buf_written; // Number of free bytes left in the buffer

                    if(buffer_avail == 0){ // Buffer is full, send error response
                        send_response(args->epollfd, req_state, args->pool, ERR_400);
                        continue;
                    }

                    // Receive request
                    if((rv = recv(req_state->connfd, req_state->buf, buffer_avail, 0)) == -1){
                        if(errno == EAGAIN || errno == EWOULDBLOCK){
                            // Rearm socket
                            ev.data.ptr = req_state;
                            if(epoll_ctl(args->epollfd, EPOLL_CTL_MOD, req_state->connfd, &ev) == -1)
                                print_log("could not rearm socket: %s\n", LOG_ERR, stderr, strerror(errno));
                        } else{
                            print_log("could not recv from socket: %s\n", LOG_ERR, stderr, strerror(errno));
                            req_state_pool_release(args->pool, req_state);
                        }

                        continue;
                    } else if(rv == 0){
                        req_state_pool_release(args->pool, req_state);
                        continue;
                    }

                    // Find end of request and NULL terminate it
                    char *end;
                    if((end = memmem(req_state->buf + req_state->buf_written, rv, "\r\n\r\n", 4)) == NULL)
                        continue;
                    *end = '\0';

                    req_state->buf_written += rv;

                    // Split request line for parsing
                    char *save_ptr, *path;
                    if((req_state->method_str = strtok_r(req_state->buf, " ", &save_ptr)) == NULL || (path = strtok_r(NULL, " ", &save_ptr)) == NULL || (req_state->version_str = strtok_r(NULL, "\r\n", &save_ptr)) == NULL){
                        print_log("malformed request line\n", LOG_ERR, stderr);
                        send_response(args->epollfd, req_state, args->pool, ERR_400);
                        continue;
                    }

                    // Detect method
                    uint8_t method;
                    if(strcmp(req_state->buf, "GET") == 0)
                        method = GET;
                    else if(strcmp(req_state->buf, "HEAD") == 0)
                        method = HEAD;
                    else{
                        send_response(args->epollfd, req_state, args->pool, ERR_405);
                        continue;
                    }

                    // Detect protocol version
                    uint8_t ver;
                    if(strcmp(req_state->version_str, "HTTP/1.1") == 0)
                        ver = HTTP_1_1;
                    else if(strcmp(req_state->version_str, "HTTP/1.0") == 0){
                        req_state->close_conn = 1;
                        ver = HTTP_1_0;
                    } else{
                        send_response(args->epollfd, req_state, args->pool, ERR_400);
                    }

                    const char *headers = req_state->version_str + 10;

                    char *host_header;
                    if(ver == HTTP_1_1 && (host_header = strcasestr(headers, "host:")) == NULL){
                        send_response(args->epollfd, req_state, args->pool, ERR_400);
                        continue;
                    }

                    if(path[0] != '/'){ // Might be absolute form URI
                        host_header += 5; // Go to value
                        while(isblank(*host_header)) host_header++; // Skip whitespace

                        char *host;
                        if(strncasecmp(path, "http://", 7) != 0 || (path = strchr((host = path + 7), '/')) == NULL || strncasecmp(host, host_header, path - host) != 0){
                            send_response(args->epollfd, req_state, args->pool, ERR_400);
                            continue;
                        }
                    }

                    // Check path len
                    if(strlen(path) > MAX_PATH_LEN){
                        send_response(args->epollfd, req_state, args->pool, ERR_414);
                        continue;
                    }

                    // If path is empty, set it to index page
                    if(path[0] == '/' && path[1] == '\0')
                        path = index_page;

                    // Open file to serve
                    static const struct open_how how = {
                        .flags = O_RDONLY,
                        .mode = 0,
                        .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV
                    };
                    if((req_state->file = syscall(__NR_openat2, AT_FDCWD, path, &how, sizeof(how))) == -1){
                        print_log("could not open file: %s\n", LOG_ERR, stderr, strerror(errno));
                        send_response(args->epollfd, req_state, args->pool, errno == ENOENT ? ERR_404 : ERR_500);
                        continue;
                    }

                    // Stat used to get file size and modification date
                    struct stat stat_buf;
                    if(fstat(req_state->file, &stat_buf) == -1){
                        print_log("could not read file length: %s\n", LOG_ERR, stderr, strerror(errno));
                        send_response(args->epollfd, req_state, args->pool, ERR_500);
                        continue;
                    }

                    // Check header values
                    char *if_modified_since;
                    if((if_modified_since = strcasestr(headers, "If-Modified-Since:")) != NULL){
                        if_modified_since += 18; // Go to value
                        while(isblank(*if_modified_since)) if_modified_since++; // Skip whitespace

                        time_t header_timestamp;
                        if((header_timestamp = string_to_timestamp(if_modified_since)) < 0){
                            send_response(args->epollfd, req_state, args->pool, ERR_400);
                            continue;
                        }

                        if(header_timestamp >= stat_buf.st_mtime){
                            send_response(args->epollfd, req_state, args->pool, ERR_304);
                            continue;
                        }
                    }

                    char *connection_header;
                    if((connection_header = strcasestr(headers, "connection:")) != NULL){
                        connection_header += 11; // Go to header value
                        while(isblank(*connection_header)) connection_header++; // Skip trailing whitespace

                        if(ver == HTTP_1_1 && strcasecmp(connection_header, "close") == 0)
                            req_state->close_conn = 1;
                        else if(ver == HTTP_1_0)
                            if(strcasecmp(connection_header, "keep-alive") == 0) req_state->close_conn = 0;
                    }

                    // Determine body mime type
                    mime_type *bsearch_rv;
                    const char *extension;

                    if((extension = strrchr(path, '.')) != NULL && (bsearch_rv = bsearch(extension, mime_types, N_MIME_TYPES, sizeof(mime_type), strcmp_bsearch_wrapper)) != NULL)
                        req_state->mimetype = bsearch_rv->mime_type;
                    else
                        req_state->mimetype = "application/octet-stream";

                    // Determine wether to send body
                    if(method == HEAD)
                        req_state->file = -1;
                    else
                        req_state->fsize = stat_buf.st_size;
                    // Send response
                    send_response(args->epollfd, req_state, args->pool, OK);
                } else if(req_state->stage == WRITE){
                    // Response is not done sending, continue
                    send_response(args->epollfd, req_state, args->pool, OK);
                }
            }
        }
    }

    pthread_exit(NULL);
}

int main(int argc, char ** argv){
    int portnum = 8080;
    int16_t backlog = BACKLOG, tnum = sysconf(_SC_NPROCESSORS_ONLN);

    // Parse CLI args
    for(int i = 1; i < argc; i+=2){
        if(argv[i][0] == '-')
            switch(argv[i][1]){
                case 'h':
                    printf("simple http server written in pure C\nusage: server [options]\n\toptions:\n\t\t-p\tPort to bind to (default: 8080)\n\t\t-m\tMaximum amount of headers accepted (default: 64)\n\t\t-b\tBacklog size (default: 128)\n\t\t-t\tNumber of threads (default: number of physical threads)\n");
                    exit(0);
                case 'p':
                    if((portnum = strtol(argv[i+1], NULL, 10)) > 65535 || portnum < 1 || errno != 0){
                        fprintf(stderr, "error: invalid port number provided. Enter a number between 1 and 65535\n");
                        return -1;
                    }

                    continue;
                case 'b':
                    if((backlog = strtol(argv[i+1], NULL, 10)) > 256 || backlog < 1 || errno != 0){
                        fprintf(stderr, "invalid backlog number. Enter a number between 1 and 256\n");
                        exit(-1);
                    }

                    continue;
                case 't':
                    if((tnum = strtol(argv[i+1], NULL, 10)) > 256 || tnum < 1 || errno != 0){
                        fprintf(stderr, "invalid thread count. Enter a number between 1 and 256\n");
                        exit(-1);
                    }

                    continue;
                case 'l':
                    if(strcmp(argv[i+1], "debug") == 0)
                        log_level = LOG_DEBUG;
                    else if(strcmp(argv[i+1], "info") == 0)
                        log_level = LOG_INFO;
                    else if(strcmp(argv[i+1], "warning") == 0)
                        log_level = LOG_WARN;
                    else if(strcmp(argv[i+1], "error") == 0)
                        log_level = LOG_ERR;
                    else
                        fprintf(stderr, "invalid log level provided. Try -h for help\n");

                    continue;
            }

        fprintf(stderr, "error: unrecognized argument \"%s\"\n", argv[i]);
        exit(-1);
    }

    // Allocate epoll event buffer
    struct epoll_event *events_buf;
    if((events_buf = malloc(MAX_EVENTS * sizeof(struct epoll_event))) == NULL){
        print_log("could not allocate epoll event buffer: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }

    // Create epoll instance
    int epollfd;
    if((epollfd = epoll_create1(0)) == -1){
        print_log("could not create epoll fd: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }

    // Start listening socket
    int listening_socket;
    if((listening_socket = startsock(portnum, backlog)) == -1){
        print_log("could not create listening socket: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }

    // Set listening socket to nonblocking
    if(setnonblocking(listening_socket) == -1)
        exit(-1);

    // Add listening socket to interest list
    // Wasting 4kb of memory here because ev.data.ptr is an union and we need it to be consistent
    struct epoll_event ev;
    if((ev.data.ptr = malloc(sizeof(request_state))) == NULL){
        print_log("could not allocate request_state struct for listening socket: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }
    (*(request_state *)ev.data.ptr).connfd = listening_socket;
    ev.events = EPOLLIN | EPOLLET;

    if(epoll_ctl(epollfd, EPOLL_CTL_ADD, listening_socket, &ev) == -1){
        print_log("could not add listening socket to interest list: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }

    // Start worker threads

    thread_args args = {.epollfd = epollfd, .listenfd = listening_socket, .events = events_buf};

    // Allocate memory pool for request state
    if((args.pool = req_state_pool_alloc(MAX_EVENTS)) == NULL){
        print_log("could not allocate request state memory pool: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }

    for(uint8_t i = 1; i < tnum; i++){ // Core number - 1 because main thread becomes worker
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, &args);
        pthread_detach(tid);
    }

    print_log("server started on port %d, number of threads: %d, log level: %s\n", LOG_INFO, stdout, portnum, tnum, log_level_str[log_level]);

    // Turn main thread into worker
    handle_client(&args);
}