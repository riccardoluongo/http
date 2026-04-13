#include "main.h"

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
        print_log("could not create socket: %s\n", LOG_ERR, stderr, strerror(errno));
        return -1;
    }

    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1){
        print_log("could not set REUSEADDR: %s\n", LOG_ERR, stderr, strerror(errno));
        close(sockfd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if(bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) != 0){
        print_log("could not bind to address: %s\n", LOG_ERR, stderr, strerror(errno));
        return -1;
    }

    if((listen(sockfd, backlog)) != 0){
        print_log("could not listen on socket: %s\n", LOG_ERR, stderr, strerror(errno));
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

int8_t sendbody(req_state_pool *pool, request_state *request, int epollfd, ssize_t *offset, ssize_t count){
    ssize_t sent;
    struct epoll_event write_ev = {epoll_write_flags, request}; // Used to rearm sockets for writes

    do{
        errno = 0;
        sent = sendfile(request->sockfd, request->body.fd, offset, count);
    } while(sent == -1 && errno == EINTR);

    if(sent <= 0){
        if(errno == EAGAIN || errno == EWOULDBLOCK){
            WRITE_REARM_OR_DIE;
        } else{
            print_log("could not send response body: %s\n", LOG_ERR, stderr, strerror(errno));
            WRITE_ERR;
        }
    }

    if(sent < count){
        WRITE_REARM_OR_DIE;
        return -1;
    }

    return 0;
}

/*  Send response to REQUEST, which is part of epoll instance args.epollfd.
    The STATUS_CODE argument will be ignored after the first function call (request is already sending)
    Return 0 when done, -1 on partial write or error
    Return values are currently unused but I'm keeping them in case I will need them for future changes */
int8_t send_response(int epollfd, request_state *request, req_state_pool *pool, uint8_t status_code){
    if(request->resp_header.sent == 0){ // Nothing sent yet, prepare response
        char time_buf[TIME_BUF_SIZE];
        get_1123_date(time_buf, TIME_BUF_SIZE);

        if(request->ranges.n_ranges == 0)
            request->status_code = status_code;
        else if(status_code == OK)
            request->status_code = PARTIAL_206;

        switch(request->status_code){
            case ERR_400:
                request->resp_header.buf_len = snprintf(request->resp_header.buf, RESPONSE_BUF_LEN, "HTTP/1.1 400 Bad Request\r\nDate: %s\r\nContent-Length: 24\r\nContent-Type: text/html%s\r\n\r\n<h1>400 Bad Request</h1>", time_buf, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case ERR_500:
                request->resp_header.buf_len = snprintf(request->resp_header.buf, RESPONSE_BUF_LEN, "HTTP/1.1 500 Internal Server Error\r\nDate: %s\r\nContent-Length: 34\r\nContent-Type: text/html%s\r\n\r\n<h1>500 Internal Server Error</h1>", time_buf, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case ERR_404:
                request->resp_header.buf_len = snprintf(request->resp_header.buf, RESPONSE_BUF_LEN, "HTTP/1.1 404 Not Found\r\nDate: %s\r\nContent-Length: 22\r\nContent-Type: text/html%s\r\n\r\n<h1>404 Not Found</h1>", time_buf, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case ERR_405:
                request->resp_header.buf_len  = snprintf(request->resp_header.buf, RESPONSE_BUF_LEN, "HTTP/1.1 405 Method Not Allowed\r\nDate: %s\r\nAllow: GET, HEAD%s\r\n\r\n", time_buf, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case ERR_414:
                request->resp_header.buf_len  = snprintf(request->resp_header.buf, RESPONSE_BUF_LEN, "HTTP/1.1 414 URI Too Long\r\nDate: %s\r\nContent-Length: 25\r\nContent-Type: text/html%s\r\n\r\n<h1>414 URI Too Long</h1>", time_buf, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case ERR_304:
                request->resp_header.buf_len = snprintf(request->resp_header.buf, RESPONSE_BUF_LEN, "HTTP/1.1 304 Not Modified\r\nDate: %s%s\r\n\r\n", time_buf, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case OK:
                request->resp_header.buf_len = snprintf(request->resp_header.buf, RESPONSE_BUF_LEN, "HTTP/1.1 200 OK\r\nDate: %s\r\nContent-Length: %ld\r\nContent-Type: %s\r\nAccept-Ranges: bytes%s\r\n\r\n", time_buf, request->body.fsize, request->mimetype, request->close_conn ? "\r\nConnection: close" : "");
                break;
            case PARTIAL_206:
                if(request->ranges.n_ranges == 1)
                    request->resp_header.buf_len = snprintf(request->resp_header.buf, RESPONSE_BUF_LEN, "HTTP/1.1 206 Partial Content\r\nDate: %s\r\nContent-Length: %ld\r\nContent-Type: %s\r\nContent-range: %ld-%ld/%ld%s\r\n\r\n", time_buf, request->ranges.total_size, request->mimetype, request->ranges.arr[0].start, request->ranges.arr[0].end, request->body.fsize, request->close_conn ? "\r\nConnection: close" : "");
                else{
                    /*  Allocate memory pool for all the data needed for the multipart response
                        offsets:
                        0 - uuid
                        37 - headers

                        remember to free this!!
                    */
                    char *uuid = request->ranges.multipart_pool = malloc(UUID_LEN + (request->ranges.n_ranges * sizeof(multipart_header_sized_str)));
                    if(uuid == NULL){
                        print_log("could not allocate multipart response memory pool: %s\n", LOG_ERR, stderr, strerror(errno));
                        exit(-1);
                    }

                    if(uuidv4_gen(uuid) == NULL){
                        print_log("could not generate UUID buffer for multipart request boundary: %s, exiting.\n", LOG_ERR, stderr, strerror(errno));
                        exit(-1);
                    }

                    // Write headers
                    multipart_header_sized_str *headers_buf = request->ranges.multipart_pool + UUID_LEN;
                    for(uint8_t i = 0; i < request->ranges.n_ranges; i++){
                        request->ranges.total_size += headers_buf[i].written = snprintf(headers_buf[i].str, MULTIPART_HEADER_LEN, i > 0 ? "\r\n--%s\r\nContent-Type: %s\r\nContent-Range: %ld-%ld/%ld\r\n\r\n" : "--%s\r\nContent-Type: %s\r\nContent-Range: %ld-%ld/%ld\r\n\r\n", (char *)uuid, request->mimetype, request->ranges.arr[i].start, request->ranges.arr[i].end, request->body.fsize);
                        headers_buf[i].sent = 0;
                    }

                    request->resp_header.buf_len = snprintf(request->resp_header.buf, RESPONSE_BUF_LEN, "HTTP/1.1 206 Partial Content\r\nDate: %s\r\nContent-Length: %ld\r\nContent-Type: multipart/byteranges; boundary=%s%s\r\n\r\n", time_buf, request->ranges.total_size, (char *)uuid, request->close_conn ? "\r\nConnection: close" : "");
                }

                break;
            default:
                print_log("unrecognized response status code\n", LOG_ERR, stderr);
                send_response(epollfd, request, pool, ERR_500);
        }

        // If is error response and a file has been opened, close it
        if(request->status_code != OK && request->status_code != PARTIAL_206 && request->body.fd > -1){
            close(request->body.fd);
            request->body.fd = -1;
        }

        request->stage = WRITE;
    }

    struct epoll_event write_ev = {epoll_write_flags, request}; // Used to rearm socket for writes
    struct epoll_event read_ev = {epoll_read_flags, request}; // Used to rearm socket for reads
    ssize_t sent;

    if(request->resp_header.sent < request->resp_header.buf_len){
        do{
            errno = 0;
            sent = send(request->sockfd, request->resp_header.buf + request->resp_header.sent, request->resp_header.buf_len - request->resp_header.sent, request->body.fd > 0 ? MSG_MORE : 0);
        } while(sent == -1 && errno == EINTR);

        if(sent <= 0){
            if(errno == EAGAIN || errno == EWOULDBLOCK)
                WRITE_REARM_OR_DIE;
            else {
                print_log("could not send response header: %s\n", LOG_ERR, stderr, strerror(errno));
                WRITE_ERR;
            }
        }

        if((request->resp_header.sent += sent) < request->resp_header.buf_len){
            WRITE_REARM_OR_DIE;
            return -1;
        }
    }

    if(request->body.fd > 0){ // If the response has a body, send it
        if(request->ranges.n_ranges == 0){
            if(sendbody(pool, request, epollfd, &request->body.fsent, request->body.fsize - request->body.fsent) == -1) return -1;
        }
        else if(request->ranges.n_ranges == 1){
            if(sendbody(pool, request, epollfd, &request->ranges.arr[0].start, request->ranges.arr[0].end - request->ranges.arr[0].start) == -1) return -1;
        } else{ // Is multipart response
            int optval; // Used to cork and uncork socket

            if(request->ranges.ranges_sent == 0){
                // Cork socket
                optval = 1;
                if(setsockopt(request->sockfd, IPPROTO_TCP, TCP_CORK, &optval, sizeof(optval)) == -1){
                    print_log("could not cork socket: %s\n", LOG_ERR, stderr, strerror(errno)); // Not sending error response as we've already sent part of the original response
                    exit(-1);
                }
            }

            while(request->ranges.ranges_sent < request->ranges.n_ranges){
                multipart_header_sized_str *headers = request->ranges.multipart_pool + UUID_LEN;

                if(headers[request->ranges.ranges_sent].sent < headers[request->ranges.ranges_sent].written){
                    do{
                        errno = 0;
                        sent = send(request->sockfd, headers[request->ranges.ranges_sent].str + headers[request->ranges.ranges_sent].sent, headers[request->ranges.ranges_sent].written - headers[request->ranges.ranges_sent].sent, 0);
                    } while(sent == -1 && errno == EINTR);

                    if(sent <= 0){
                        if(errno == EAGAIN || errno == EWOULDBLOCK)
                            WRITE_REARM_OR_DIE;
                        else {
                            print_log("could not send response header: %s\n", LOG_ERR, stderr, strerror(errno));
                            WRITE_ERR;
                        }
                    }

                    if((headers[request->ranges.ranges_sent].sent += sent) < headers[request->ranges.ranges_sent].written){
                        WRITE_REARM_OR_DIE;
                        return -1;
                    }
                }

                if(sendbody(pool, request, epollfd, &request->ranges.arr[request->ranges.ranges_sent].start, request->ranges.arr[request->ranges.ranges_sent].end - request->ranges.arr[request->ranges.ranges_sent].start) == -1)
                    return -1;

                request->ranges.ranges_sent++;
            }

            // Uncork socket
            optval = 0;
            if(setsockopt(request->sockfd, IPPROTO_TCP, TCP_CORK, &optval, sizeof(optval)) == -1){
                print_log("could not uncork socket: %s\n", LOG_ERR, stderr, strerror(errno));
                exit(-1);
            }

            // Free memory pool
            free(request->ranges.multipart_pool);
        }

        close(request->body.fd);
    }


    if(request->status_code == ERR_500)
        exit(-1);

    // Reset request state
    reset_state(request);

    if(request->close_conn)
        close(request->sockfd);
    else if(epoll_ctl(epollfd, EPOLL_CTL_MOD, request->sockfd, &read_ev) == -1){
        print_log("could not rearm socket: %s\n", LOG_ERR, stderr, strerror(errno));
        return -1;
    }

    return 0;
}

// Parse and store value(s) from the Range header.
// Return 0 on success and -1 on failure
// This function's code is really messy and probably needs improvement
int8_t parse_range(char *header_value, request_state *request){
    uint8_t more; // True if there are more ranges to parse
    char *next_value = header_value;

    do{
        if(request->ranges.n_ranges >= MAX_RANGES) return -1; // Maximum amount of ranges exceeded, ignore them

        header_value = next_value;
        more = (next_value = strchr(header_value, ',')) == NULL ? 0 : 1;
        next_value++; // Skip comma

        char *separator;
        if((separator = strchr(header_value, '-')) == NULL)
            return -1;

        // Whitespace is only allowed if there are multiple values
        if(request->ranges.n_ranges > 0)
            while(isblank(*header_value)) header_value++;
        else
            if(isblank(*header_value)) return -1;

        errno = 0; // Needed for strtol error handling
        char *endptr; // Used to check for illegal characters in values

        if(separator == header_value){ // Separator is at the start of the value, this is a suffix range
            if(
                isspace(*(header_value + 1)) ||
                (request->ranges.arr[request->ranges.n_ranges].end = strtol(header_value + 1, &endptr, 10)) > request->body.fsize ||
                ((*endptr != '\r' && *(endptr + 1) != '\n') && (more && (!isblank(*endptr) && *endptr != ','))) ||
                errno == EINVAL || errno == ERANGE
            ) return -1;

            // Normalize range
            request->ranges.arr[request->ranges.n_ranges].start = request->body.fsize - request->ranges.arr[request->ranges.n_ranges].end;
            request->ranges.arr[request->ranges.n_ranges].end = request->body.fsize;
        } else if((*(separator + 1) == '\r' && *(separator + 2) == '\n') || (more && (isblank(*separator + 1) || *(separator + 1) == ','))){ // Is open-ended range
            if(
                isspace(*header_value) ||
                (request->ranges.arr[request->ranges.n_ranges].start = strtol(header_value, &endptr, 10)) >= request->body.fsize ||
                endptr != separator ||
                errno == EINVAL || errno == ERANGE
            ) return -1;

            // Normalize range
            request->ranges.arr[request->ranges.n_ranges].end = request->body.fsize;
        } else{ // Check if normal range
            if(
                (request->ranges.n_ranges == 0 && isspace(*header_value)) || // Whitespace before the value is only illegal if this is the first range (e.g. "bytes= 400-600")
                (request->ranges.arr[request->ranges.n_ranges].start = strtol(header_value, &endptr, 10)) >= request->body.fsize ||
                endptr != separator || // There must be no whitespace after the number, so the separator has to come immediately after
                errno == EINVAL || errno == ERANGE
            ) return -1;

            errno = 0;

            if(
                isspace(*header_value) || // Whitespace after the separator is always illegal
                (request->ranges.arr[request->ranges.n_ranges].end = strtol(separator + 1, NULL, 10)) > request->body.fsize ||
                request->ranges.arr[request->ranges.n_ranges].start >= request->ranges.arr[request->ranges.n_ranges].end ||
                errno == EINVAL || errno == ERANGE
            ) return -1;
        }

        request->ranges.total_size += request->ranges.arr[request->ranges.n_ranges].end - request->ranges.arr[request->ranges.n_ranges++].start;
    } while(more);

    return 0;
}

// Main server logic executed by workers
void * server(void *arg){
    thread_args * args = (thread_args *)arg; // Thread arguments struct passed via ARG
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
            request_state *request = (request_state *)args->events[i].data.ptr;

            if(request->sockfd == args->listenfd){ // New connection
                struct sockaddr addr;
                socklen_t addrlen = sizeof(addr);

                while(1){
                    // Accept connection
                    int sockfd;
                    if((sockfd = accept(request->sockfd, &addr, &addrlen)) == -1){
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
                    ((request_state *)ev.data.ptr)->sockfd = sockfd;
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
                if(request->stage == READ){
                    uint16_t buffer_avail = REQ_BUF_SIZE - request->req_buf.written; // Number of free bytes left in the buffer

                    if(buffer_avail == 0){ // Buffer is full, send error response
                        send_response(args->epollfd, request, args->pool, ERR_400);
                        continue;
                    }

                    // Receive request
                    int16_t sent;
                    do
                        sent = recv(request->sockfd, request->req_buf.buf, buffer_avail, 0);
                    while(sent == -1 && errno == EINTR);

                    if(sent == -1){
                        if(errno == EAGAIN || errno == EWOULDBLOCK){
                            // Rearm socket
                            ev.data.ptr = request;
                            if(epoll_ctl(args->epollfd, EPOLL_CTL_MOD, request->sockfd, &ev) == -1)
                                print_log("could not rearm socket: %s\n", LOG_ERR, stderr, strerror(errno));
                        } else{
                            print_log("could not recv from socket: %s\n", LOG_ERR, stderr, strerror(errno));
                            req_state_pool_release(args->pool, request);
                        }

                        continue;
                    } else if(sent == 0){
                        req_state_pool_release(args->pool, request);
                        continue;
                    }

                    // Find end of request and NULL terminate it
                    char *end;
                    if((end = memmem(request->req_buf.buf + request->req_buf.written, sent, "\r\n\r\n", 4)) == NULL)
                        continue;
                    *(end + 4) = '\0';

                    request->req_buf.written += sent;

                    // Split request line for parsing
                    char *save_ptr, *path;
                    if((request->method_str = strtok_r(request->req_buf.buf, " ", &save_ptr)) == NULL || (path = strtok_r(NULL, " ", &save_ptr)) == NULL || (request->version_str = strtok_r(NULL, "\r\n", &save_ptr)) == NULL){
                        print_log("malformed request line\n", LOG_ERR, stderr);
                        send_response(args->epollfd, request, args->pool, ERR_400);
                        continue;
                    }

                    // Detect method
                    uint8_t method;
                    if(strcmp(request->req_buf.buf, "GET") == 0)
                        method = GET;
                    else if(strcmp(request->req_buf.buf, "HEAD") == 0)
                        method = HEAD;
                    else{
                        send_response(args->epollfd, request, args->pool, ERR_405);
                        continue;
                    }

                    // Detect protocol version
                    uint8_t ver;
                    if(strcmp(request->version_str, "HTTP/1.1") == 0)
                        ver = HTTP_1_1;
                    else if(strcmp(request->version_str, "HTTP/1.0") == 0){
                        request->close_conn = 1;
                        ver = HTTP_1_0;
                    } else{
                        send_response(args->epollfd, request, args->pool, ERR_400);
                    }

                    const char *headers = request->version_str + 10;

                    char *host_header;
                    if(ver == HTTP_1_1 && (host_header = strcasestr(headers, "host:")) == NULL){
                        send_response(args->epollfd, request, args->pool, ERR_400);
                        continue;
                    }

                    if(path[0] != '/'){ // Might be absolute form URI
                        host_header += 5; // Go to value
                        while(isblank(*host_header)) host_header++; // Skip whitespace

                        char *host;
                        if(strncasecmp(path, "http://", 7) != 0 || (path = strchr((host = path + 7), '/')) == NULL || strncasecmp(host, host_header, path - host) != 0){
                            send_response(args->epollfd, request, args->pool, ERR_400);
                            continue;
                        }
                    }

                    // Check path len
                    if(strlen(path) > MAX_PATH_LEN){
                        send_response(args->epollfd, request, args->pool, ERR_414);
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
                    if((request->body.fd = syscall(__NR_openat2, args->root_fd, path, &how, sizeof(how))) == -1){
                        print_log("could not open '%s' file: %s\n", LOG_ERR, stderr, path, strerror(errno));
                        send_response(args->epollfd, request, args->pool, errno == ENOENT ? ERR_404 : ERR_500);
                        continue;
                    }

                    // Stat used to get file size and modification date
                    struct stat stat_buf;
                    if(fstat(request->body.fd, &stat_buf) == -1){
                        print_log("could not read file length: %s\n", LOG_ERR, stderr, strerror(errno));
                        send_response(args->epollfd, request, args->pool, ERR_500);
                        continue;
                    }

                    // Determine wether to send body
                    if(method == HEAD)
                        request->body.fd = -1;
                    else
                        request->body.fsize = stat_buf.st_size;

                    // Check header values
                    char *if_modified_since;
                    if((if_modified_since = strcasestr(headers, "If-Modified-Since:")) != NULL){
                        if_modified_since += 18; // Go to value
                        while(isblank(*if_modified_since)) if_modified_since++; // Skip whitespace

                        time_t header_timestamp;
                        if((header_timestamp = string_to_timestamp(if_modified_since)) < 0){
                            send_response(args->epollfd, request, args->pool, ERR_400);
                            continue;
                        }

                        if(header_timestamp >= stat_buf.st_mtime){
                            send_response(args->epollfd, request, args->pool, ERR_304);
                            continue;
                        }
                    }

                    char *range;
                    if((range = strcasestr(headers, "range:")) != NULL){
                        range += 6; // Skip header name
                        while(isblank(*range)) range++; // Skip whitespace

                        if(strncmp(range, "bytes=", 6) == 0 && parse_range(range += 6, request) == -1)
                            request->ranges.n_ranges = 0;
                    }

                    char *connection_header;
                    if((connection_header = strcasestr(headers, "connection:")) != NULL){
                        connection_header += 11; // Go to header value
                        while(isblank(*connection_header)) connection_header++; // Skip trailing whitespace

                        if(ver == HTTP_1_1 && strcasecmp(connection_header, "close") == 0)
                            request->close_conn = 1;
                        else if(ver == HTTP_1_0)
                            if(strcasecmp(connection_header, "keep-alive") == 0) request->close_conn = 0;
                    }

                    // Determine body mime type
                    mime_type *bsearch_rv;
                    const char *extension;

                    if((extension = strrchr(path, '.')) != NULL && (bsearch_rv = bsearch(extension, mime_types, N_MIME_TYPES, sizeof(mime_type), strcmp_bsearch_wrapper)) != NULL)
                        request->mimetype = bsearch_rv->mime_type;
                    else
                        request->mimetype = "application/octet-stream";


                    // Send response
                    send_response(args->epollfd, request, args->pool, OK);
                } else if(request->stage == WRITE){
                    // Response is not done sending, continue
                    send_response(args->epollfd, request, args->pool, OK);
                }
            }
        }
    }

    pthread_exit(NULL);
}

int main(int argc, char ** argv){
    char *server_dir = NULL;
    int portnum = 8080;
    int16_t backlog = BACKLOG, tnum = sysconf(_SC_NPROCESSORS_ONLN);
    thread_args args;

    // Parse CLI args
    for(int i = 1; i < argc; i+=2){
        if(argv[i][0] == '-')
            switch(argv[i][1]){
                case 'h':
                    printf("simple multithreaded http server written in C.\nusage: server [-option] [value]\n\toptions:\n\t\t-p\tPort to bind to (default: 8080)\n\t\t-b\tBacklog size (default: 128)\n\t\t-t\tNumber of threads (default: number of physical threads)\n\t\t-l\tLog level (default: info, possible values: debug, info, warning, error)\n\t\t-d\tDirectory to serve files from. Files to serve are to be placed in this directory. (default: executable's directory)\n");
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
                case 'd':
                    server_dir = argv[i+1];
                    continue;
            }

        fprintf(stderr, "error: unrecognized argument \"%s\"\n", argv[i]);
        exit(-1);
    }

    // Allocate epoll event buffer
    if((args.events = malloc(MAX_EVENTS * sizeof(struct epoll_event))) == NULL){
        print_log("could not allocate epoll event buffer: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }

    // Create epoll instance
    if((args.epollfd = epoll_create1(0)) == -1){
        print_log("could not create epoll fd: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }

    // Start listening socket
    if((args.listenfd = startsock(portnum, backlog)) == -1){
        print_log("could not create listening socket: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }

    // Set listening socket to nonblocking
    if(setnonblocking(args.listenfd) == -1)
        exit(-1);

    // Add listening socket to interest list
    // Wasting 4kb of memory here because ev.data.ptr is an union and we need it to be consistent
    struct epoll_event ev;
    if((ev.data.ptr = malloc(sizeof(request_state))) == NULL){
        print_log("could not allocate request_state struct for listening socket: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }
    (*(request_state *)ev.data.ptr).sockfd = args.listenfd;
    ev.events = EPOLLIN | EPOLLET;

    if(epoll_ctl(args.epollfd, EPOLL_CTL_ADD, args.listenfd, &ev) == -1){
        print_log("could not add listening socket to interest list: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }

    // Allocate memory pool for request state
    if((args.pool = req_state_pool_alloc(MAX_EVENTS)) == NULL){
        print_log("could not allocate request state memory pool: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }

    if(server_dir == NULL){ // No server folder supplied, use default
        // Find server executable path
        static char server_path[MAX_PATH_LEN];
        ssize_t path_len = readlink("/proc/self/exe", server_path, MAX_PATH_LEN - 1);
        if(path_len < 0){
            print_log("could not read server directory: %s\n", LOG_ERR, stderr, strerror(errno));
            exit(-1);
        }

        server_path[path_len] = '\0';
        server_dir = dirname(server_path);
    }

    if((args.root_fd = open(server_dir, O_RDONLY | O_DIRECTORY)) < 0){
        print_log("could not open server directory: %s\n", LOG_ERR, stderr, strerror(errno));
        exit(-1);
    }

    // Start worker threads
    for(uint8_t i = 1; i < tnum; i++){ // Core number - 1 because main thread becomes worker
        pthread_t tid;
        pthread_create(&tid, NULL, server, &args);
        pthread_detach(tid);
    }

    print_log("server started on port %d, number of threads: %d, log level: %s, serving from directory: %s\n", LOG_INFO, stdout, portnum, tnum, log_level_str[log_level], server_dir);

    // Turn main thread into worker
    server(&args);
}