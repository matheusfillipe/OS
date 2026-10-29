#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define ARRAY_LEN(a)     (sizeof(a) / sizeof((a)[0]))
#define PAGE_SIZE        4096UL

#define barrier() __asm__ volatile("" ::: "memory")

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Logs to every attached sink (serial, framebuffer console). */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* kprintf with the trailing newline added. */
#define KLOG(fmt, ...) kprintf(fmt "\n", ##__VA_ARGS__)
__attribute__((noreturn)) void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void halt_forever(void);

#define KASSERT(cond)                                                          \
    do {                                                                       \
        if (!(cond))                                                           \
            panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__);  \
    } while (0)
