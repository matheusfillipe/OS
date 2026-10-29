#pragma once

/* Linux x86-64 system call numbers -> implementations. Numbers come from
 * arch/x86/entry/syscalls/syscall_64.tbl and must never change. */

#define SYSCALLS(X)                                                                                      \
    X(0, read) X(1, write) X(2, open) X(3, close) X(4, stat) X(5, fstat) X(6, lstat) X(7, poll)            \
    X(8, lseek) X(9, mmap) X(10, mprotect) X(11, munmap) X(12, brk) X(13, rt_sigaction)                    \
    X(14, rt_sigprocmask) X(15, rt_sigreturn) X(16, ioctl) X(17, pread64) X(18, pwrite64) X(19, readv)     \
    X(20, writev) X(21, access) X(22, pipe) X(24, sched_yield) X(25, mremap) X(26, msync)                  \
    X(28, madvise) X(32, dup) X(33, dup2) X(34, pause) X(35, nanosleep) X(39, getpid) X(40, sendfile) X(41, socket) X(42, connect) X(43, accept) X(44, sendto) X(45, recvfrom) \
    X(46, sendmsg) X(47, recvmsg) X(48, shutdown) X(49, bind) X(50, listen) X(51, getsockname)              \
    X(52, getpeername) X(54, setsockopt) X(55, getsockopt) X(288, accept4) X(56, clone)        \
    X(57, fork) X(58, vfork) X(59, execve) X(60, exit) X(61, wait4) X(62, kill) X(63, uname)              \
    X(72, fcntl) X(73, flock) X(74, fsync) X(75, fsync) X(76, truncate) X(77, ftruncate)                  \
    X(79, getcwd) X(80, chdir) X(81, fchdir) X(82, rename) X(83, mkdir) X(84, rmdir) X(85, creat)        \
    X(86, link) X(87, unlink) X(88, symlink) X(89, readlink) X(90, chmod) X(91, fchmod) X(92, chown)      \
    X(93, fchown) X(94, lchown) X(95, umask) X(96, gettimeofday) X(97, getrlimit) X(98, getrusage)          \
    X(99, sysinfo) X(100, times) X(102, getuid) X(103, syslog) X(104, getgid) X(105, setuid)              \
    X(106, setgid) X(107, geteuid) X(108, getegid) X(109, setpgid) X(110, getppid) X(111, getpgrp)       \
    X(112, setsid) X(115, getgroups) X(116, setgroups) X(113, setreuid) X(114, setregid) X(117, setresuid) X(118, getresuid) X(119, setresgid) X(120, getresgid) X(121, getpgid) X(122, setfsuid) X(123, setfsgid) X(124, getsid) X(125, capget) X(126, capset) X(127, rt_sigpending)               \
    X(130, rt_sigsuspend) X(131, sigaltstack) X(137, statfs) X(138, fstatfs) X(140, getpriority) X(141, setpriority) X(142, sched_setparam) X(143, sched_getparam) X(144, sched_setscheduler) X(145, sched_getscheduler) X(157, prctl)                \
    X(158, arch_prctl) X(160, setrlimit) X(162, sync) X(169, reboot) X(170, sethostname) X(155, pivot_root) X(161, chroot) X(165, mount) X(166, umount2) X(272, unshare) X(308, setns) X(317, seccomp)                \
    X(186, gettid) X(200, tkill) X(201, time) X(202, futex) X(203, sched_setaffinity) X(204, sched_getaffinity)                    \
    X(217, getdents64) X(218, set_tid_address) X(228, clock_gettime) X(229, clock_getres)               \
    X(230, clock_nanosleep) X(231, exit_group) X(234, tgkill) X(257, openat) X(258, mkdirat)            \
    X(260, fchownat) X(262, newfstatat) X(263, unlinkat) X(264, renameat) X(265, linkat) X(266, symlinkat) \
    X(267, readlinkat) X(268, fchmodat) X(269, faccessat) X(271, ppoll) X(273, set_robust_list)         \
    X(280, utimensat) X(292, dup3) X(293, pipe2) X(302, prlimit64) X(316, renameat2) X(318, getrandom)  \
    X(332, statx) X(435, clone3) X(436, close_range) X(439, faccessat2) X(307, sendmmsg)                                                     \
    X(23, select) X(53, socketpair) X(213, epoll_create) X(232, epoll_wait) X(233, epoll_ctl)           \
    X(270, pselect6) X(281, epoll_pwait) X(282, signalfd) X(283, timerfd_create) X(284, eventfd)        \
    X(285, fallocate) X(286, timerfd_settime) X(287, timerfd_gettime) X(289, signalfd4)                 \
    X(290, eventfd2) X(291, epoll_create1) X(319, memfd_create) X(441, epoll_pwait2) X(306, syncfs)    \
    X(149, mlock) X(150, mlock) X(151, mlock) X(152, mlock) X(325, mlock) X(221, fadvise64) X(135, personality)

#define NR_read 0
#define NR_write 1
#define NR_exit 60
#define NR_rt_sigreturn 15
#define NR_execve 59
#define NR_rseq 334
