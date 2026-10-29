#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HZ 250
extern volatile uint64_t global_ticks;

enum thread_state { THREAD_READY, THREAD_RUNNING, THREAD_SLEEPING, THREAD_BLOCKED, THREAD_DEAD };

struct thread {
    uint64_t rsp; /* saved kernel stack pointer; must stay first (switch.S) */
    uint64_t id;
    enum thread_state state;
    int last_cpu;
    uint8_t *kstack;
    uint64_t kstack_size;
    uint64_t wake_tick;
    bool wake_pending;
    void (*entry)(void *);
    void *arg;
    struct thread *next; /* run queue / sleep list link */
    struct user_thread *user; /* NULL for kernel-only threads */
    uint64_t switch_reserve;  /* bytes kept free at the stack top (user frame) */
    char name[32];
};

/* Called once per CPU from that CPU's boot context, which becomes its idle
 * thread. */
void sched_init_cpu(void);

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg);
/* Like thread_create, but keeps `reserve` bytes at the top of the kernel
 * stack (for a user register frame) and attaches user state. The thread is
 * not started until thread_start_ready(). */
struct thread *thread_create_ex(const char *name, void (*entry)(void *), void *arg, uint64_t reserve,
                                struct user_thread *user);
void thread_start_ready(struct thread *t);
static inline uint64_t thread_kstack_top(struct thread *t) { return (uint64_t)t->kstack + t->kstack_size; }
__attribute__((noreturn)) void thread_exit(void);
void thread_yield(void);
void thread_sleep_ms(uint64_t ms);
struct thread *thread_current(void);

/* Blocking primitive for wait queues: block_current() parks the running
 * thread; thread_wake() makes it runnable again. */
void thread_block_current(void);
/* Blocks until thread_wake() or until global_ticks reaches `tick` (0: no
 * timeout). Either way the caller re-checks what it waits for. */
void thread_block_until(uint64_t tick);
void thread_wake(struct thread *t);

/* Timer interrupt hook. */
void sched_tick(void);
