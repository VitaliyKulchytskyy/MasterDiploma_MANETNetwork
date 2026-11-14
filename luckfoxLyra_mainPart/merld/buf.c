#include "buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void buf_printf(struct buf* b, const char* fmt, ...) {
    for (;;) {
        va_list args;
        va_start(args, fmt);
        const size_t room = b->capacity - b->length;
        const int n = vsnprintf(b->data ? b->data + b->length : NULL, room, fmt, args);
        va_end(args);
        if (n < 0)
            return;
        if ((size_t)n < room) {
            b->length += (size_t)n;
            return;
        }
        const size_t capacity = (b->capacity + (size_t)n + 1) * 2;
        char* data = realloc(b->data, capacity);
        if (!data)
            return;
        b->data = data;
        b->capacity = capacity;
    }
}

void buf_json_string(struct buf* b, const uint8_t* text, size_t length) {
    buf_printf(b, "\"");
    for (size_t i = 0; i < length; ++i) {
        const uint8_t c = text[i];
        if (c == '"' || c == '\\')
            buf_printf(b, "\\%c", c);
        else if (c == '\n')
            buf_printf(b, "\\n");
        else if (c == '\r')
            buf_printf(b, "\\r");
        else if (c < 0x20 || c == 0x7f)
            buf_printf(b, "\\u%04x", c);
        else
            buf_printf(b, "%c", c);
    }
    buf_printf(b, "\"");
}

void buf_free(struct buf* b) {
    free(b->data);
    *b = (struct buf){0};
}
