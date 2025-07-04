#ifndef THREADS_H_
#define THREADS_H_

#include "user/setjmp.h"

#define NULL_FUNC ((void (*)(int)) - 1)
#define NOT_SUSPENDED 0
#define SUSPENDED 1
#define NO_SIGNAL (-1)
#define SIGNAL_ZERO 0
#define SIGNAL_ONE 1
#define STACK_SIZE (0x100 * sizeof(unsigned long))

struct thread
{
    void (*fp)(void *arg);
    void *arg;
    void *stack;
    void *stack_p;
    void *handler_stack;
    void *handler_stack_p;
    jmp_buf env;
    int buf_set;
    int ID;
    struct thread *previous;
    struct thread *next;

    void (*sig_handler[2])(int);
    int sent;

    int pending_signal;

    int suspended;
    jmp_buf handler_env;
    int handler_buf_set;

    int in_handler;
};

struct thread *thread_create(void (*f)(void *), void *arg);
void thread_add_runqueue(struct thread *t);
void thread_yield(void);
void dispatch(void);
void schedule(void);
void thread_exit(void);
void thread_start_threading(void);
struct thread *get_current_thread(void);

void thread_register_handler(int signo, void (*f)(int));
void thread_kill(struct thread *t, int signo);
void thread_resume(struct thread *t);
void thread_suspend(struct thread *t);

#endif
