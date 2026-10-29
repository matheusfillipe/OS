#pragma once
#include <stdint.h>

/* The parts of the Linux user ABI that are specific to x86-64: they differ
 * from the asm-generic layout that arm64 uses. */

#define O_DIRECTORY 0200000
#define O_NOFOLLOW 0400000

#define SA_RESTORER 0x04000000

/* clone's argument order: flags, newsp, parent_tid, child_tid, tls. */
#define CLONE_ARGS(a1, a2, a3, a4, a5) (a1), (a2), (a3), (a4), (a5)

struct linux_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

struct linux_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    struct linux_timespec st_atim;
    struct linux_timespec st_mtim;
    struct linux_timespec st_ctim;
    int64_t __unused[3];
};
