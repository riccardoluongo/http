#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "types.h"

#define FNV_OFFSET 14695981039346656037UL
#define FNV_PRIME 1099511628211UL

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
    "content-type",
    "cookie",
    "prefer",
};

uint64_t hash(const char* key) {
    uint64_t hash = FNV_OFFSET;
    for (const char* p = key; *p; p++) {
        hash ^= (uint64_t)(unsigned char)(*p);
        hash *= FNV_PRIME;
    }
    return hash;
}

void ht_free(ht *table){
    if(table == NULL)
    	return;

    free(table->arr);
    free(table);
}

ht* ht_alloc(uint8_t max_headers){
    ht* table = calloc(1, sizeof(ht));

    table->capacity = max_headers * 8;

    if((table->arr = calloc(table->capacity, sizeof(Header))) == NULL)
        return NULL;

    return table;
}

int compare(const void *s1, const void *s2){
    return(strcmp(s1, s2));
}

// Look for an header in the hash table and return a pointer to it if it exists, otherwise NULL.
// KEY must be all lowercase.
Header * get_header(ht *table, char *key){
    uint16_t i, starting_index, totalsize = 0;
    int8_t val_index, rv, n_values;
    char *value;

    i = starting_index = hash(key) % table->capacity;

    if(table->arr[i].key == NULL)
        return NULL;

    while(strcmp(key, table->arr[i].key) != 0)
        if((i = (i + 1) % table->capacity) == starting_index || table->arr[i].key == NULL)
            return NULL;

    if(bsearch(key, combinable_headers, 23, sizeof(char *), compare) != NULL)
        table->arr[i].can_combine = 1;
    else
        table->arr[i].can_combine = 0;

    return &table->arr[i];
}

// Add header to hash table. Return 0 on success or one of the following error codes:
//      -1: table is full
//      -2: invalid header
int8_t set_header(ht* table, char* headerptr){
    char *val;
    uint16_t i, starting_index;
    int8_t rv;

    if((val = strchr(headerptr, ':')) == NULL) // Find the start of the header value
        return -2;
    *val = '\0';

    do ++val; while(isspace(*val)); // Skip LWS

    for(char *strptr = headerptr; *strptr; strptr++) // Convert header key to lowercase
        *strptr = tolower(*strptr);
    i = starting_index = hash(headerptr) % table->capacity;

    while(table->arr[i].key != NULL && strcmp(table->arr[i].key, headerptr) != 0)
        if((i = (i+1) % table->capacity) == starting_index)
            return -1;

    if(table->arr[i].n_values + 1 > MAX_HEADER_VALUES)
        return -1;

    table->arr[i].key = headerptr;
    table->arr[i].values[table->arr[i].n_values++] = val;

    return 0;
}
