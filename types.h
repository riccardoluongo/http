#include <stddef.h>
#include <stdint.h>
#include <arpa/inet.h>

#define MAX_HEADERS 64
#define N_MIME_TYPES 20
#define DEFAULT_HEADERS_LEN 512
#define ERR_RESPONSE_LEN 256
#define MAX_HEADER_VALUES 16
#define BACKLOG 32
#define ERR_400 0
#define ERR_500 1
#define ERR_404 2
#define ERR_304 3
#define GET_METHOD 0
#define HEAD_METHOD 1
#define QUEUE_CAPACITY 1024

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERR
} LOG_LEVEL;

typedef struct Header {
    char *key;
    char **values;
    void *next;

    uint8_t n_values; // Specifies the number of values the header has. This will only be above one if the duplicated headers can be combined safely
    uint8_t semicolon_separated; // Specifies wether the header's values should be separated with a semicolon (1) or with a comma (0)
} Header;

typedef struct{
    Header *arr;
    uint8_t items;
    uint16_t capacity;
} ht;

typedef struct {
    const char *extension;
    const char *mime_type;
} MimeType;

struct date_enum{
    char *name;
    uint8_t num;
};

ht * ht_alloc(uint8_t max_headers);
void ht_free(ht *table);
char * get_header(ht* table, char* key);
int8_t set_header(ht* table, char* header);
int8_t headercheck(const char *key);
