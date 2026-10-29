#pragma once
/* Linux x86-64 user ABI: the constants and structure layouts that user
 * programs (musl, glibc, busybox, ...) were compiled against. Values must
 * match Linux exactly; they come from the Linux UAPI headers' definitions. */
#include <stdint.h>

/* errno */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define ENOTBLK 15
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOLCK 37
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EMLINK 31
#define EPIPE 32
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40
#define ENOTSUP 95

/* open flags */
#define O_ACCMODE 03
#define O_RDONLY 00
#define O_WRONLY 01
#define O_RDWR 02
#define O_CREAT 0100
#define O_EXCL 0200
#define O_NOCTTY 0400
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW 0400000
#define O_CLOEXEC 02000000
#define O_PATH 010000000

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_FOLLOW 0x400
#define AT_EMPTY_PATH 0x1000

/* file types (st_mode) */
#define S_IFMT 0170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000

/* d_type */
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* fcntl */
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1
#define F_UNLCK 2

/* futex */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_BITSET_MATCH_ANY 0xffffffffu

/* mmap */
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_GROWSDOWN 0x0100
#define MAP_NORESERVE 0x4000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_SHARED_VALIDATE 0x03
#define MAP_FAILED ((uint64_t)-1)
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2
#define MREMAP_DONTUNMAP 4
#define MADV_DONTNEED 4

/* clone */
#define CSIGNAL 0x000000ff
#define CLONE_VM 0x00000100
#define CLONE_FS 0x00000200
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_VFORK 0x00004000
#define CLONE_PARENT 0x00008000
#define CLONE_THREAD 0x00010000
#define CLONE_SYSVSEM 0x00040000
#define CLONE_SETTLS 0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID 0x01000000

/* wait */
#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8

/* signals */
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGSYS 31
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGWINCH 28
#define NSIG 65 /* signals are 1..64 (Linux _NSIG + 1); sets keep signal n in bit n-1 */
#define SIG_DFL 0
#define SIG_IGN 1
#define SA_SIGINFO 0x00000004
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER 0x04000000
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

struct siginfo {
    int32_t si_signo, si_errno, si_code, pad;
    uint8_t rest[112];
};

/* ioctl */
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCSCTTY 0x540E
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD 0x541B
#define FIONBIO 0x5421

/* poll */
#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLWRNORM 0x100
#define POLLRDHUP 0x2000

/* epoll (event bits are the POLL* values) */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP (1u << 29)
#define EPOLLONESHOT (1u << 30)
#define EPOLLET (1u << 31)

struct linux_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

/* eventfd, timerfd, signalfd, memfd */
#define EFD_SEMAPHORE 1
#define TFD_TIMER_ABSTIME 1
#define TFD_TIMER_CANCEL_ON_SET 2
#define MFD_CLOEXEC 0x1
#define MFD_ALLOW_SEALING 0x2
#define MFD_HUGETLB 0x4
#define MFD_NOEXEC_SEAL 0x8
#define MFD_EXEC 0x10
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL 0x01
#define F_SEAL_SHRINK 0x02
#define F_SEAL_GROW 0x04
#define F_SEAL_WRITE 0x08
#define F_SEAL_FUTURE_WRITE 0x10
#define F_SEAL_EXEC 0x20

struct linux_signalfd_siginfo {
    uint32_t signo;
    int32_t errno_;
    int32_t code;
    uint32_t pid, uid;
    int32_t fd;
    uint32_t tid, band, overrun, trapno;
    int32_t status, int_;
    uint64_t ptr, utime, stime, addr;
    uint16_t addr_lsb;
    uint8_t pad[46];
};

/* clocks */
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME 7

/* reboot */
#define LINUX_REBOOT_CMD_RESTART 0x01234567
#define LINUX_REBOOT_CMD_HALT 0xCDEF0123
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDC

/* auxv */
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_EXECFN 31

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct linux_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct linux_itimerspec {
    struct linux_timespec interval;
    struct linux_timespec value;
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

struct linux_statx_timestamp {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t __reserved;
};

struct linux_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct linux_statx_timestamp stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor;
    uint32_t stx_dev_major, stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t __spare2[13];
};

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
} __attribute__((packed));

struct linux_iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

struct linux_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct linux_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

struct linux_termios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line;
    uint8_t c_cc[19];
};

/* termios2 (TCGETS2...): the same fields plus explicit speeds. glibc 2.42+
 * uses these for tcgetattr/tcsetattr. */
#define TCGETS2 0x802C542A
#define TCSETS2 0x402C542B
#define TCSETSW2 0x402C542C
#define TCSETSF2 0x402C542D
struct linux_termios2 {
    struct linux_termios t;
    uint32_t c_ispeed, c_ospeed;
};

struct linux_winsize {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
};

struct linux_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct linux_rlimit {
    uint64_t rlim_cur, rlim_max;
};

struct linux_sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs, pad;
    uint32_t pad2;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
    char _f[4];
};

/* termios bits we interpret */
#define TC_ICRNL 0000400
#define TC_OPOST 0000001
#define TC_ONLCR 0000004
#define TC_ISIG 0000001
#define TC_ICANON 0000002
#define TC_ECHO 0000010
#define TC_ECHOE 0000020
#define TC_ECHOK 0000040
#define TC_ECHOCTL 0001000
#define TC_IEXTEN 0100000
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSUSP 10

/* socket errno values */
#define ENOTSOCK 88
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define EPROTOTYPE 91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENETUNREACH 101
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EALREADY 114
#define EINPROGRESS 115

/* sockets */
#define AF_UNIX 1
#define AF_NETLINK 16
#define AF_INET 2
#define AF_INET6 10
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000
#define SOL_SOCKET 1
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_PASSCRED 16
#define SO_PEERCRED 17
#define SO_ACCEPTCONN 30
#define SO_PROTOCOL 38
#define SO_DOMAIN 39
#define SCM_RIGHTS 1
#define SCM_CREDENTIALS 2
#define MSG_PEEK 0x02
#define MSG_CTRUNC 0x08
#define MSG_TRUNC 0x20
#define MSG_DONTWAIT 0x40
#define MSG_WAITALL 0x100
#define MSG_NOSIGNAL 0x4000
#define MSG_CMSG_CLOEXEC 0x40000000
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

struct linux_cmsghdr {
    uint64_t len;
    int32_t level;
    int32_t type;
};

struct linux_ucred {
    int32_t pid;
    uint32_t uid, gid;
};

struct linux_sockaddr_in {
    uint16_t family;
    uint16_t port; /* network order */
    uint32_t addr; /* network order */
    uint8_t zero[8];
};

struct linux_msghdr {
    uint64_t name;
    uint32_t namelen;
    uint32_t pad0;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t control;
    uint64_t controllen;
    int32_t flags;
    int32_t pad1;
};
