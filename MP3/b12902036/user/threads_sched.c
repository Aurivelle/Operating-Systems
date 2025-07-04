#include "kernel/types.h"
#include "user/user.h"
#include "user/list.h"
#include "user/threads.h"
#include "user/threads_sched.h"
#include <limits.h>
#define NULL 0

#ifdef THREAD_SCHEDULER_DEFAULT
struct threads_sched_result schedule_default(struct threads_sched_args args)
{
    struct thread *thread_with_smallest_id = NULL;
    struct thread *th = NULL;
    list_for_each_entry(th, args.run_queue, thread_list)
    {
        if (thread_with_smallest_id == NULL || th->ID < thread_with_smallest_id->ID)
            thread_with_smallest_id = th;
    }

    struct threads_sched_result r;
    if (thread_with_smallest_id != NULL)
    {
        r.scheduled_thread_list_member = &thread_with_smallest_id->thread_list;
        r.allocated_time = thread_with_smallest_id->remaining_time;
    }
    else
    {
        r.scheduled_thread_list_member = args.run_queue;
        r.allocated_time = 1;
    }

    return r;
}
#endif

#ifdef THREAD_SCHEDULER_HRRN
struct threads_sched_result schedule_hrrn(struct threads_sched_args args)
{
    struct threads_sched_result r;
    struct thread *best = NULL;
    struct thread *th = NULL;

    unsigned long best_num = 0, best_den = 1;

    list_for_each_entry(th, args.run_queue, thread_list)
    {
        unsigned long wait = args.current_time - th->arrival_time;
        unsigned long num = wait + th->processing_time;
        unsigned long den = th->processing_time;

        if (!best || num * best_den > best_num * den || (num * best_den == best_num * den && th->ID < best->ID))
        {
            best = th;
            best_num = num;
            best_den = den;
        }
    }

    if (best)
    {
        r.scheduled_thread_list_member = &best->thread_list;

        r.allocated_time = best->remaining_time;
    }
    else
    {
        int next_rel = INT_MAX;
        struct release_queue_entry *rq;
        list_for_each_entry(rq, args.release_queue, thread_list)
        {
            if (rq->release_time > args.current_time &&
                rq->release_time < next_rel)
                next_rel = rq->release_time;
        }

        r.scheduled_thread_list_member = args.run_queue;
        r.allocated_time = (next_rel != INT_MAX)
                               ? (next_rel - args.current_time)
                               : 1;
        return r;
    }

    return r;
}
#endif

#ifdef THREAD_SCHEDULER_PRIORITY_RR

struct threads_sched_result schedule_priority_rr(struct threads_sched_args args)
{
    struct threads_sched_result r;
    struct thread *th = NULL;
    int min_prio = INT_MAX;

    list_for_each_entry(th, args.run_queue, thread_list)
    {
        if (th->priority < min_prio)
        {
            min_prio = th->priority;
        }
    }

    int grp_size = 0;
    list_for_each_entry(th, args.run_queue, thread_list)
    {
        if (th->priority == min_prio)
            grp_size++;
    }

    list_for_each_entry(th, args.run_queue, thread_list)
    {
        if (th->priority == min_prio)
        {
            r.scheduled_thread_list_member = &th->thread_list;

            if (grp_size > 1)
            {
                r.allocated_time = th->remaining_time < args.time_quantum
                                       ? th->remaining_time
                                       : args.time_quantum;
            }
            else
            {
                r.allocated_time = th->remaining_time;
            }
            return r;
        }
    }

    r.scheduled_thread_list_member = args.run_queue;
    r.allocated_time = 1;
    return r;
}
#endif

#if defined(THREAD_SCHEDULER_EDF_CBS) || defined(THREAD_SCHEDULER_DM)
static struct thread *__check_deadline_miss(struct list_head *run_queue, int current_time)
{
    struct thread *th = NULL;
    struct thread *thread_missing_deadline = NULL;
    list_for_each_entry(th, run_queue, thread_list)
    {
        if (th->current_deadline <= current_time)
        {
            if (thread_missing_deadline == NULL)
                thread_missing_deadline = th;
            else if (th->ID < thread_missing_deadline->ID)
                thread_missing_deadline = th;
        }
    }
    return thread_missing_deadline;
}
#endif

#ifdef THREAD_SCHEDULER_DM

static int __dm_thread_cmp(struct thread *a, struct thread *b)
{
    if (a->period < b->period)
        return -1;
    if (a->period > b->period)
        return 1;

    if (a->ID < b->ID)
        return -1;
    if (a->ID > b->ID)
        return 1;
    return 0;
}

struct threads_sched_result schedule_dm(struct threads_sched_args args)
{
    struct threads_sched_result r;
    struct thread *th, *best = NULL;

    struct thread *missed = __check_deadline_miss(args.run_queue, args.current_time);
    if (missed)
    {
        r.scheduled_thread_list_member = &missed->thread_list;
        r.allocated_time = 0;
        return r;
    }

    int next_rel = INT_MAX;
    struct release_queue_entry *rq = NULL;
    list_for_each_entry(rq, args.release_queue, thread_list)
    {
        if (rq->release_time > args.current_time && rq->release_time < next_rel)
        {
            next_rel = rq->release_time;
        }
    }

    if (list_empty(args.run_queue))
    {
        r.scheduled_thread_list_member = args.run_queue;
        if (next_rel != INT_MAX)
            r.allocated_time = next_rel - args.current_time;
        else
            r.allocated_time = 1;
        return r;
    }

    list_for_each_entry(th, args.run_queue, thread_list)
    {
        if (!best || __dm_thread_cmp(th, best) < 0)
        {
            best = th;
        }
    }

    int allocated = best->remaining_time;

    if (next_rel != INT_MAX)
    {
        int until = next_rel - args.current_time;
        if (allocated > until)
            allocated = until;
    }

    if (args.current_time + allocated > best->current_deadline)
    {
        r.scheduled_thread_list_member = &best->thread_list;
        r.allocated_time = 1;
        return r;
    }

    r.scheduled_thread_list_member = &best->thread_list;
    r.allocated_time = allocated;
    return r;
}
#endif

#ifdef THREAD_SCHEDULER_EDF_CBS

static int __edf_thread_cmp(struct thread *a, struct thread *b)
{
    if (a->current_deadline < b->current_deadline)
        return -1;
    if (a->current_deadline > b->current_deadline)
        return 1;
    if (a->ID < b->ID)
        return -1;
    if (a->ID > b->ID)
        return 1;
    return 0;
}

static inline int is_cbs_soft(struct thread *t)
{
    return !t->cbs.is_hard_rt;
}

#define MIN(a, b) ((a) < (b) ? (a) : (b))

struct threads_sched_result schedule_edf_cbs(struct threads_sched_args args)
{
    struct threads_sched_result res;
    struct thread *th;

    list_for_each_entry(th, args.run_queue, thread_list)
    {

        if (
            th->cbs.remaining_budget <= 0 &&
            th->remaining_time > 0)
        {
            th->cbs.is_throttled = 1;
        }
    }

    list_for_each_entry(th, args.run_queue, thread_list)
    {
        if (th->cbs.is_throttled && args.current_time == th->current_deadline)
        {
            th->cbs.is_throttled = 0;
            th->cbs.remaining_budget = th->cbs.budget;
            th->current_deadline += th->period;
        }
    }

    struct thread *miss = __check_deadline_miss(args.run_queue, args.current_time);
    if (miss)
    {
        if (!is_cbs_soft(miss))
        {

            res.scheduled_thread_list_member = &miss->thread_list;
            res.allocated_time = 0;
            return res;
        }
        else
        {

            miss->cbs.is_throttled = 0;
            miss->cbs.remaining_budget = miss->cbs.budget;
            miss->current_deadline = args.current_time + miss->period;
        }
    }

    if (list_empty(args.run_queue))
    {
        int next_event = INT_MAX;

        struct release_queue_entry *rq;
        list_for_each_entry(rq, args.release_queue, thread_list)
        {
            if (rq->release_time > args.current_time && rq->release_time < next_event)
                next_event = rq->release_time;
        }

        list_for_each_entry(th, args.run_queue, thread_list)
        {
            if (th->cbs.is_throttled && th->current_deadline > args.current_time &&
                th->current_deadline < next_event)
                next_event = th->current_deadline;
        }

        int sleep = (next_event == INT_MAX) ? 1 : (next_event - args.current_time);
        if (sleep <= 0)
            sleep = 1;
        res.scheduled_thread_list_member = args.run_queue;
        res.allocated_time = sleep;
        return res;
    }

    struct thread *best = NULL;

    while (1)
    {
        best = NULL;

        list_for_each_entry(th, args.run_queue, thread_list)
        {
            if (th->cbs.is_throttled)
                continue;

            if (!best || __edf_thread_cmp(th, best) < 0)
                best = th;
        }

        if (!best)
            break;

        if (is_cbs_soft(best))
        {
            int dist = best->current_deadline - args.current_time;
            if (dist > 0)
            {
                long lhs = (long)best->cbs.remaining_budget * best->period;
                long rhs = (long)best->cbs.budget * dist;
                if (lhs > rhs)
                {
                    // Postpone: extend deadline and reset budget
                    best->current_deadline = args.current_time + best->period;
                    best->cbs.remaining_budget = best->cbs.budget;
                    continue; // restart selection
                }
            }
        }

        break; // best is valid
    }

    if (!best)
    {
        int next_event = INT_MAX;

        struct release_queue_entry *rq;
        list_for_each_entry(rq, args.release_queue, thread_list) if (rq->release_time > args.current_time && rq->release_time < next_event)
            next_event = rq->release_time;

        list_for_each_entry(th, args.run_queue, thread_list)
        {
            if (th->cbs.is_throttled && th->current_deadline > args.current_time &&
                th->current_deadline < next_event)
                next_event = th->current_deadline;
        }

        int sleep = (next_event == INT_MAX) ? 1 : (next_event - args.current_time);
        if (sleep <= 0)
            sleep = 1;
        res.scheduled_thread_list_member = args.run_queue;
        res.allocated_time = sleep;
        return res;
    }

    int alloc = best->remaining_time;

    int gap_dl = best->current_deadline - args.current_time;
    alloc = MIN(alloc, gap_dl);

    if (is_cbs_soft(best))
        alloc = MIN(alloc,
                    best->cbs.remaining_budget > 0 ? best->cbs.remaining_budget : 1);

    int next_rel = INT_MAX, next_rel_dl = INT_MAX;
    struct release_queue_entry *rq;
    list_for_each_entry(rq, args.release_queue, thread_list)
    {
        if (rq->release_time > args.current_time &&
            rq->release_time < next_rel)
        {
            next_rel = rq->release_time;
            next_rel_dl = rq->release_time + rq->thrd->deadline;
        }
        else if (rq->release_time == next_rel)
        {
            int cand_dl = rq->release_time + rq->thrd->deadline;
            if (cand_dl < next_rel_dl)
                next_rel_dl = cand_dl;
        }
    }
    if (next_rel != INT_MAX && next_rel > args.current_time)
    {
        int incoming_is_better = 0;
        if (next_rel_dl < best->current_deadline)
            incoming_is_better = 1;
        else if (next_rel_dl == best->current_deadline &&
                 rq->thrd->ID < best->ID) // ID tie‑break
            incoming_is_better = 1;

        if (incoming_is_better)
            alloc = MIN(alloc, next_rel - args.current_time);
    }

    int next_pre = INT_MAX;
    list_for_each_entry(th, args.run_queue, thread_list) if (is_cbs_soft(th) && th->cbs.is_throttled &&
                                                             th->current_deadline < best->current_deadline &&
                                                             th->current_deadline > args.current_time &&
                                                             th->current_deadline < next_pre)
        next_pre = th->current_deadline;
    if (next_pre != INT_MAX)
        alloc = MIN(alloc, next_pre - args.current_time);

    if (alloc <= 0)
        alloc = 1;

    res.scheduled_thread_list_member = &best->thread_list;
    res.allocated_time = alloc;
    return res;
}

#endif
