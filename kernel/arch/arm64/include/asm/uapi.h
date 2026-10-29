#pragma once
#include <stdint.h>

/* The parts of the Linux user ABI that arm64 takes from asm-generic, where
 * they differ from x86-64. */

#define O_DIRECTORY 040000
#define O_NOFOLLOW 0100000

/* clone's argument order: flags, newsp, parent_tid, tls, child_tid. */
#define CLONE_ARGS(a1, a2, a3, a4, a5) (a1), (a2), (a3), (a5), (a4)

struct linux_epoll_event {
    uint32_t events;
    uint64_t data;
};

struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t st_size;
    int32_t st_blksize;
    int32_t __pad2;
    int64_t st_blocks;
    struct linux_timespec st_atim;
    struct linux_timespec st_mtim;
    struct linux_timespec st_ctim;
    uint32_t __unused[2];
};
