#pragma once
#include <stdbool.h>
#include <stddef.h>

#define KUKU_CONSOLE_LINE_CAP 384
typedef struct {
    char text[KUKU_CONSOLE_LINE_CAP];
    size_t used;
    bool invalid;
} kuku_console_line_t;
typedef enum { KUKU_LINE_MORE, KUKU_LINE_READY, KUKU_LINE_REJECTED } kuku_line_result_t;
// A READY line stays valid until the next byte. Reject an entire overlong or
// NUL-containing command, rather than executing its truncated prefix.
kuku_line_result_t kuku_console_line_feed(kuku_console_line_t *line, unsigned char byte);
