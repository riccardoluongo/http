#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "types.h"

#define FNV_OFFSET 14695981039346656037UL
#define FNV_PRIME 1099511628211UL

// Return the hash of KEY
static uint64_t hash(const char* key) {
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

    Header *entry;

    for(uint16_t i = 0; i < table->capacity; i++){
        entry = &table->arr[i];

        free(entry->values);

        while((entry = entry->next) != NULL){
            free(entry->values);
            free(entry);
        }
    }

    free(table->arr);
    free(table);
}

// Allocate space for the headers hash table.
ht* ht_alloc(uint8_t max_headers){
    ht* table = calloc(1, sizeof(ht));

    table->capacity = max_headers * 8;

    if((table->arr = calloc(table->capacity, sizeof(Header))) == NULL)
        return NULL;

    return table;
}

// Look for an header in the hash table and return a pointer to its value if it exists, otherwise NULL.
// KEY must be all lowercase.
// The memory pointed to by the pointer returned by this function must be freed after use.
// TODO separate return val for error
char * get_header(ht* table, char* key){
    size_t i = hash(key) % table->capacity;
    Header *entry = &table->arr[i];
    uint16_t totalsize = 0;
    uint8_t val_index;
    char *value;

    if(entry->key == NULL)
        return NULL;

    // Search through linked list until the right key is found
    while(strcmp(key, entry->key) != 0){
        if(entry->key == NULL)
            return NULL;

        entry = entry->next;
    }

    for(val_index = 0; val_index < entry->n_values; val_index++)
        totalsize += strlen(entry->values[val_index]);

    if((value = malloc(totalsize + (2 * entry->n_values - 1))) == NULL)
        return NULL;

    strcpy(value, *entry->values);
    for(val_index = 1; val_index < entry->n_values; val_index++){
        strcat(value, entry->values[val_index]);
        strcat(value, entry->semicolon_separated ? "; " : ", ");
    }

    return value;
}

int8_t add_header_value(Header *entry, char *val){
    if(entry->n_values > 15)
        return -1;
    entry->values[entry->n_values++] = val;

    return 0;
}

// Add value to header based on headercheck.
int8_t add_header_value_with_check(Header *entry, char *key, char *val){
    int8_t rv;

    if((rv = headercheck(val)) >= 1){ // Header value can be combined
        entry->semicolon_separated = rv == 1 ? 0 : 2;
        if(add_header_value(entry, val) == -1) return -4;
   } else if (rv == -1) // Header value must not be combined, return an error
        return -3;
    else{ // Header value should not be combined, replace the it with the latest one
        if((entry->values = malloc(sizeof(char *))) == NULL)
            return -1;

        *entry->values = val;
    }

    return 0;
}

// Add header to hash table. Return 0 on success or one of the following error codes:
// -1 on memory allocation errors
// -2 on parsing errors
// -3 if the header is duplicated and can't be combined
// -4 on maximum amount of headers exceeded
int8_t set_header(ht* table, char* headerptr){
    char *val;

    if((val = strchr(headerptr, ':')) == NULL) // Find the start of the header value
        return -2;
    *val = '\0';

    do ++val; while(isspace(*val)); // Skip LWS

    for(char *strptr = headerptr; *strptr; strptr++) // Convert header key to lowercase
        *strptr = tolower(*strptr);

    uint16_t i = hash(headerptr) % table->capacity; // Calculate the index for the key
    Header *entry = &table->arr[i];

    if(entry->key == NULL){ // No header in this position, use it
        if((entry->values = malloc(MAX_HEADER_VALUES * sizeof(char *))) == NULL)
            return -1;

        entry->key = headerptr;
        if(add_header_value(entry, val) == -1) return -1;
    } else if(strcmp(entry->key, headerptr) == 0){ // Header with this key already exists in table
        int8_t rv;

        if((rv = headercheck(headerptr)) >= 1){ // Header value can be combined
            entry->semicolon_separated = rv == 1 ? 0 : 2;
            if(add_header_value(entry, val) == -1) return -1;
        } else if (rv == -1) // Header value must not be combined, return an error
            return -3;
        else // Header value should not be combined, replace the it with the latest one
            *entry->values = val;
    } else{ // Hash collision found
        int8_t rv, header_exists = 0;

        // Follow linked list while checking if we find a duplicated header
        while((entry = entry->next) != NULL)
            if(strcmp(entry->key, headerptr) == 0){
                if((rv = add_header_value_with_check(entry, headerptr, val)) < 0)
                    return rv;

                header_exists = 1;
                break;
            }

        if(!header_exists){ // This is a new header, allocate space for it and store it
            if((entry = malloc(sizeof(Header))) == NULL)
                return -1;

            entry->key = headerptr;

            if((entry->values = malloc(MAX_HEADER_VALUES * sizeof(char *))) == NULL)
                return -1;

            if(add_header_value(entry, val) == -1) return -4;
        }
    }

    table->items++;

    return 0;
}
