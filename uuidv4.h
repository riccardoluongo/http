#include <stdint.h>
#include <sys/random.h>
#include <stdio.h>

// 128 bits to store the UUID in numerical form
typedef struct {
    uint64_t high, low;
} uuid_t;

char * uuidv4_gen(char *buf);