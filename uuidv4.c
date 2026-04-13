#include "uuidv4.h"

// Generate a UUIDv4 and store it as a string inside BUF as per RFC 9562.
// BUF must be at least 37 bytes wide, and at most 37 bytes will be written.
// Return a pointer to BUF on success or NULL on failure
char * uuidv4_gen(char *buf){
    uuid_t uuid;

    if(getrandom(&uuid, sizeof(uuid_t), 0) == -1)
        return NULL;

    // Set version field to 4 (bits 58 through 51 from the left)
    uuid.high = (uuid.high & 0xFFFFFFFFFFFF0FFF) | (4UL << 12);

    // Set variant field to 2 (bits 64 and 65)
    uuid.low = (uuid.low & 0x3FFFFFFFFFFFFFFF) | (2UL << 62);

    // Convert to string
    snprintf(buf, 37, "%08lx-%04lx-%04lx-%04lx-%012lx", (uuid.high >> 32) & 0x00000000FFFFFFFF, (uuid.high >> 16) & 0x000000000000FFFF, uuid.high & 0x000000000000FFFF, (uuid.low >> 48) & 0x000000000000FFFF, uuid.low & 0x0000FFFFFFFFFFFF);

    return buf;
}