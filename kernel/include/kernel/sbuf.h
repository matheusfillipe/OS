#pragma once
#include <stdbool.h>
#include <stddef.h>

/* A growable text buffer for generated file contents. */
struct sbuf {
    char *buf;
    size_t len, cap;
    bool oom;
};
void sb_put(struct sbuf *b, const char *s, size_t n);
void sb_printf(struct sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Pads the current line with spaces up to column `col` (no %-Ns in ksnprintf). */
void sb_pad(struct sbuf *b, size_t col);
void sb_free(struct sbuf *b);
