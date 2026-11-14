#ifndef MERLD_BUF_H
#define MERLD_BUF_H

// Growable string buffer, used to build the JSON answers and state files

#include <stddef.h>
#include <stdint.h>

struct buf {
    char* data;
    size_t length;
    size_t capacity;
};

__attribute__((format(printf, 2, 3)))
void buf_printf(struct buf* b, const char* fmt, ...);

// Append text as a quoted JSON string, escaping quotes, backslashes and control characters
void buf_json_string(struct buf* b, const uint8_t* text, size_t length);

void buf_free(struct buf* b);

#endif // MERLD_BUF_H
