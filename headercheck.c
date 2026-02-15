#include "types.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const char *combinable_headers[] = {
    "a-im",
    "accept",
    "accept-charset",
    "accept-encoding",
    "accept-language",
    "access-control-request-headers",
    "cache-control",
    "connection",
    "content-encoding",
    "expect",
    "forwarded",
    "if-match",
    "if-none-match",
    "range",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
    "via",
    "warning",
};

const char *semicolon_combinable_headers[] = {
    "content-type",
    "cookie",
    "prefer",
};

const char *must_not_combine[] = {
    "accept-datetime",
    "access-control-request-method",
    "authorization",
    "content-length",
    "content-md5",
    "date",
    "from",
    "host",
    "http2-settings",
    "if-modified-since",
    "if-range",
    "if-unmodified-since",
    "max-forwards",
    "origin",
    "pragma",
    "proxy-authorization",
    "referer",
    "user-agent"
};

int compare(const void *s1, const void *s2){
    return(strcmp(*(const char **)s1, *(const char **)s2));
}

/*
Check if the duplicated header can be combined in a comma or semicolon separated list.
Returns 1 if comma separated, 2 if semicolon separated, -1 if the header MUST NOT be combined and 0 for every other header
*/
int8_t headercheck(const char *key){
    if(bsearch(key, combinable_headers, 20, sizeof(char *), compare) != NULL)
        return 1;
    else if(bsearch(key, semicolon_combinable_headers, 3, sizeof(char *), compare) != NULL)
        return 2;
    else if(bsearch(key, must_not_combine, 18, sizeof(char *), compare) != NULL)
        return -1;
    else
        return 0;
}