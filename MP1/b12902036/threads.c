#include "kernel/types.h"
#include "user/setjmp.h"
#include "user/threads.h"
#include "user/user.h"
#include <stddef.h>

static struct thread *current_thread = NULL;
static int id = 1;

static jmp_buf env_st;
static jmp_buf env_tmp;
static jmp_buf handler_env_tmp;

struct thread *get_current_thread(void)
{
    return current_thread;
}

void thread_trampoline(void)
{
    current_thread->fp(current_thread->arg);
    thread_exit();
}

void signal_trampoline()
{
    int signo = current_thread->pending_signal;
    current_thread->sig_handler[signo](signo);
    current_thread->pending_signal = NO_SIGNAL;

    if (current_thread->buf_set)
    {
        longjmp(current_thread->env, 1);
    }
    else
    {
        env_tmp->sp = (unsigned long)current_thread->stack_p;
        env_tmp->ra = (unsigned long)thread_trampoline;
        longjmp(env_tmp, 1);
    }
}

struct thread *thread_create(void (*f)(void *), void *arg)
{
    struct thread *t = (struct thread *)malloc(sizeof(struct thread));

    t->fp = f;
    t->arg = arg;
    t->ID = id++;
    t->buf_set = 0;
    t->stack = (unsigned long)malloc(sizeof(unsigned long) * 0x100);
    t->stack_p = t->stack + 0x100 * 8 - 0x2 * 8;

    t->suspended = NOT_SUSPENDED;
    t->pending_signal = NO_SIGNAL;
    t->handler_buf_set = 0;
    t->in_handler = 0;
    unsigned long handler_new_stack = (unsigned long)malloc(sizeof(unsigned long) * 0x100);
    unsigned long handler_new_stack_p = handler_new_stack + 0x100 * 8 - 0x2 * 8;
    t->handler_stack = (void *)handler_new_stack;
    t->handler_stack_p = (void *)handler_new_stack_p;
    t->sent = 0;

    if (current_thread)
    {
        t->sig_handler[0] = current_thread->sig_handler[0];
        t->sig_handler[1] = current_thread->sig_handler[1];
    }
    else
    {
        t->sig_handler[0] = NULL_FUNC;
        t->sig_handler[1] = NULL_FUNC;
    }
    return t;
}

void thread_add_runqueue(struct thread *t)
{

    if (current_thread == NULL)
    {
        current_thread = t;
        current_thread->next = current_thread;
        current_thread->previous = current_thread;

        t->sig_handler[0] = NULL_FUNC;
        t->sig_handler[1] = NULL_FUNC;
    }
    else
    {

        struct thread *tail = current_thread->previous;
        tail->next = t;
        t->previous = tail;
        t->next = current_thread;
        current_thread->previous = t;

        t->sig_handler[0] = current_thread->sig_handler[0];
        t->sig_handler[1] = current_thread->sig_handler[1];
    }
}

void thread_yield(void)
{
    int ret;

    if (current_thread->pending_signal != NO_SIGNAL)
    {
        ret = setjmp(current_thread->handler_env);
        if (ret == 0)
        {
            current_thread->handler_buf_set = 1;
            schedule();
            dispatch();
        }
        else
        {
            current_thread->handler_buf_set = 0;
            return;
        }
    }
    else
    {

        ret = setjmp(current_thread->env);
        if (ret == 0)
        {
            current_thread->buf_set = 1;
            schedule();
            dispatch();
        }
        else
        {
            current_thread->buf_set = 0;
            return;
        }
    }
}

void schedule(void)
{
    do
    {
        current_thread = current_thread->next;
    } while (current_thread->suspended == SUSPENDED);
}

void dispatch(void)
{
    if (current_thread == NULL)
    {
        longjmp(env_st, 1);
    }

    while (current_thread->suspended == SUSPENDED)
    {
        current_thread = current_thread->next;
    }

    if (current_thread->pending_signal != NO_SIGNAL)
    {
        int s = current_thread->pending_signal;

        if (current_thread->sig_handler[s] == NULL_FUNC)
        {
            thread_exit();
        }

        if (current_thread->handler_buf_set == 0)
        {
            handler_env_tmp->sp = (unsigned long)current_thread->handler_stack_p;
            handler_env_tmp->ra = (unsigned long)signal_trampoline;
            longjmp(handler_env_tmp, 1);
        }
        else
        {
            longjmp(current_thread->handler_env, 1);
        }
    }
    if (current_thread->buf_set == 0)
    {
        current_thread->buf_set = 1;
        env_tmp->sp = (unsigned long)current_thread->stack_p;
        env_tmp->ra = (unsigned long)thread_trampoline;
        longjmp(env_tmp, 1);
    }
    else
    {
        longjmp(current_thread->env, 1);
    }
}

void thread_exit(void)
{

    if (current_thread->next != current_thread)
    {
        struct thread *temp = current_thread;
        temp->previous->next = temp->next;
        temp->next->previous = temp->previous;
        current_thread = temp->next;

        free(temp->stack);
        free(temp);

        dispatch();
    }
    else
    {

        free(current_thread->stack);
        free(current_thread);
        current_thread = NULL;
        longjmp(env_st, 1);
    }
}

void thread_start_threading(void)
{
    if (current_thread == NULL)
    {
        return;
    }

    if (setjmp(env_st) == 0)
    {
        dispatch();
    }
}

void thread_register_handler(int signo, void (*handler)(int))
{
    current_thread->sig_handler[signo] = handler;
}

void thread_kill(struct thread *t, int signo)
{
    if (t->sent == 0)
    {
        t->pending_signal = signo;
        t->sent = 1;
    }
}

void thread_suspend(struct thread *t)
{
    t->suspended = SUSPENDED;
    if (t == current_thread)
    {
        thread_yield();
    }
}

void thread_resume(struct thread *t)
{
    t->suspended = NOT_SUSPENDED;
}
