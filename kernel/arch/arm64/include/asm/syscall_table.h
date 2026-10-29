#pragma once

/* Linux arm64 system call numbers -> implementations. Numbers come from
 * include/uapi/asm-generic/unistd.h and must never change. arm64 numbers only
 * the newer forms (openat, newfstatat, clone, pipe2, dup3, ppoll, pselect6,
 * ...), which its C libraries use. */

#define SYSCALLS(X)                                                                                      \
    X(17, getcwd) X(19, eventfd2) X(20, epoll_create1) X(21, epoll_ctl) X(22, epoll_pwait) X(23, dup)      \
    X(24, dup3) X(25, fcntl) X(29, ioctl) X(32, flock) X(34, mkdirat) X(35, unlinkat) X(36, symlinkat)     \
    X(37, linkat) X(38, renameat) X(39, umount2) X(40, mount) X(41, pivot_root) X(43, statfs)              \
    X(44, fstatfs) X(45, truncate) X(46, ftruncate) X(47, fallocate) X(48, faccessat) X(49, chdir)         \
    X(50, fchdir) X(51, chroot) X(52, fchmod) X(53, fchmodat) X(54, fchownat) X(55, fchown) X(56, openat) \
    X(57, close) X(59, pipe2) X(61, getdents64) X(62, lseek) X(63, read) X(64, write) X(65, readv)        \
    X(66, writev) X(67, pread64) X(68, pwrite64) X(71, sendfile) X(72, pselect6) X(73, ppoll)             \
    X(74, signalfd4) X(78, readlinkat) X(79, newfstatat) X(80, fstat) X(81, sync) X(82, fsync)            \
    X(83, fsync) X(85, timerfd_create) X(86, timerfd_settime) X(87, timerfd_gettime) X(88, utimensat)      \
    X(90, capget) X(91, capset) X(92, personality) X(93, exit) X(94, exit_group) X(96, set_tid_address)   \
    X(97, unshare) X(98, futex) X(99, set_robust_list) X(101, nanosleep) X(113, clock_gettime)            \
    X(114, clock_getres) X(115, clock_nanosleep) X(116, syslog) X(118, sched_setparam)                     \
    X(119, sched_setscheduler) X(120, sched_getscheduler) X(121, sched_getparam)                           \
    X(122, sched_setaffinity) X(123, sched_getaffinity) X(124, sched_yield) X(129, kill) X(130, tkill)    \
    X(131, tgkill) X(132, sigaltstack) X(133, rt_sigsuspend) X(134, rt_sigaction) X(135, rt_sigprocmask) \
    X(136, rt_sigpending) X(139, rt_sigreturn) X(140, setpriority) X(141, getpriority) X(142, reboot)     \
    X(143, setregid) X(144, setgid) X(145, setreuid) X(146, setuid) X(147, setresuid) X(148, getresuid)  \
    X(149, setresgid) X(150, getresgid) X(151, setfsuid) X(152, setfsgid) X(153, times) X(154, setpgid)  \
    X(155, getpgid) X(156, getsid) X(157, setsid) X(158, getgroups) X(159, setgroups) X(160, uname)      \
    X(161, sethostname) X(163, getrlimit) X(164, setrlimit) X(165, getrusage) X(166, umask) X(167, prctl) \
    X(169, gettimeofday) X(172, getpid) X(173, getppid) X(174, getuid) X(175, geteuid) X(176, getgid)    \
    X(177, getegid) X(178, gettid) X(179, sysinfo) X(198, socket) X(199, socketpair) X(200, bind)        \
    X(201, listen) X(202, accept) X(203, connect) X(204, getsockname) X(205, getpeername) X(206, sendto) \
    X(207, recvfrom) X(208, setsockopt) X(209, getsockopt) X(210, shutdown) X(211, sendmsg)               \
    X(212, recvmsg) X(214, brk) X(215, munmap) X(216, mremap) X(220, clone) X(221, execve) X(222, mmap)   \
    X(223, fadvise64) X(226, mprotect) X(227, msync) X(228, mlock) X(229, mlock) X(230, mlock)           \
    X(231, mlock) X(233, madvise) X(242, accept4) X(260, wait4) X(261, prlimit64) X(267, syncfs)          \
    X(268, setns) X(269, sendmmsg) X(276, renameat2) X(277, seccomp) X(278, getrandom) X(279, memfd_create) \
    X(284, mlock) X(291, statx) X(435, clone3) X(436, close_range) X(439, faccessat2) X(441, epoll_pwait2)

#define NR_read 63
#define NR_write 64
#define NR_exit 93
#define NR_rt_sigreturn 139
#define NR_execve 221
#define NR_rseq 293
