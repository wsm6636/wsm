/*
 * litmus/sched_gsn_edf.c
 *
 * Implementation of the GSN-EDF scheduling algorithm.
 *
 * This version uses the simple approach and serializes all scheduling
 * decisions by the use of a queue lock. This is probably not the
 * best way to do it, but it should suffice for now.
 */

#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <litmus/debug_trace.h>
#include <litmus/litmus.h>
#include <litmus/jobs.h>
#include <litmus/sched_plugin.h>
#include <litmus/edf_common.h>
#include <litmus/sched_trace.h>
#include <litmus/trace.h>

#include <litmus/preempt.h>
#include <litmus/budget.h>
#include <litmus/np.h>

#include <litmus/bheap.h>

#ifdef CONFIG_SCHED_CPU_AFFINITY
#include <litmus/affinity.h>
#endif

/* to set up domain/cpu mappings */
#include <litmus/litmus_proc.h>

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/syscalls.h>
#include <asm/unistd.h>
#include <asm/uaccess.h>
#include <linux/buffer_head.h>
/* Overview of GSN-EDF_M operations.
 *
 * For a detailed explanation of GSN-EDF have a look at the FMLP paper. This
 * description only covers how the individual operations are implemented in
 * LITMUS.
 *
 * link_task_to_cpu(T, cpu) 	- Low-level operation to update the linkage
 *                                structure (NOT the actually scheduled
 *                                task). If there is another linked task To
 *                                already it will set To->linked_on = NO_CPU
 *                                (thereby removing its association with this
 *                                CPU). However, it will not requeue the
 *                                previously linked task (if any). It will set
 *                                T's state to not completed and check whether
 *                                it is already running somewhere else. If T
 *                                is scheduled somewhere else it will link
 *                                it to that CPU instead (and pull the linked
 *                                task to cpu). T may be NULL.
 *
 * unlink(T)			- Unlink removes T from all scheduler data
 *                                structures. If it is linked to some CPU it
 *                                will link NULL to that CPU. If it is
 *                                currently queued in the gsnedf queue it will
 *                                be removed from the rt_domain. It is safe to
 *                                call unlink(T) if T is not linked. T may not
 *                                be NULL.
 *
 * requeue(T)			- Requeue will insert T into the appropriate
 *                                queue. If the system is in real-time mode and
 *                                the T is released already, it will go into the
 *                                ready queue. If the system is not in
 *                                real-time mode is T, then T will go into the
 *                                release queue. If T's release time is in the
 *                                future, it will go into the release
 *                                queue. That means that T's release time/job
 *                                no/etc. has to be updated before requeu(T) is
 *                                called. It is not safe to call requeue(T)
 *                                when T is already queued. T may not be NULL.
 *
 * gsnedf_job_arrival(T)	- This is the catch all function when T enters
 *                                the system after either a suspension or at a
 *                                job release. It will queue T (which means it
 *                                is not safe to call gsnedf_job_arrival(T) if
 *                                T is already queued) and then check whether a
 *                                preemption is necessary. If a preemption is
 *                                necessary it will update the linkage
 *                                accordingly and cause scheduled to be called
 *                                (either with an IPI or need_resched). It is
 *                                safe to call gsnedf_job_arrival(T) if T's
 *                                next job has not been actually released yet
 *                                (releast time in the future). T will be put
 *                                on the release queue in that case.
 *
 * curr_job_completion()	- Take care of everything that needs to be done
 *                                to prepare the current task for its next
 *                                release and place it in the right queue with
 *                                gsnedf_job_arrival().
 *
 *
 * When we now that T is linked to CPU then link_task_to_cpu(NULL, CPU) is
 * equivalent to unlink(T). Note that if you unlink a task from a CPU none of
 * the functions will automatically propagate pending task from the ready queue
 * to a linked task. This is the job of the calling function ( by means of
 * __take_ready).
 */


/* cpu_entry_t - maintain the linked and scheduled state
 */
typedef struct  {
	int 			cpu;
	struct task_struct*	linked;		/* only RT tasks */
	struct task_struct*	scheduled;	/* only RT tasks */
	struct bheap_node*	hn;

//	int 			cur_budget;    /* currently available budget */
//	int 			mem_master;	/* memguard master cpu*/
//	int			task_budget;
} cpu_entry_t;
DEFINE_PER_CPU(cpu_entry_t, gsnedfm_cpu_entries);

cpu_entry_t* gsnedfm_cpus[NR_CPUS];


/* the cpus queue themselves according to priority in here */
static struct bheap_node gsnedfm_heap_node[NR_CPUS];
static struct bheap      gsnedfm_cpu_heap;

static rt_domain_t gsnedfm;
#define gsnedfm_lock (gsnedfm.ready_lock)
static void gsnedfm_task_block(struct task_struct *t);
asmlinkage long sys_get_rt_task_param(pid_t pid, struct rt_task __user * param);
extern int get_membudget(int get_cpu,int get_membudget);
extern int get_master;
extern int clean_budget(int g_cpu);
extern int get_cur_budget(void);
//extern int get_taskbudget;

/* Uncomment this if you want to see all scheduling decisions in the
 * TRACE() log.*/
#define WANT_ALL_SCHED_EVENTS
 

/*
static int get_edf(cpu_entry_t *meminfo){
        meminfo->cur_budget=get_curbudget;
        TRACE("get_master==%d,cur_budget==%d\n",get_master,meminfo->cur_budget);
	return 0;
}
*/
static int cpu_lower_prio(struct bheap_node *_a, struct bheap_node *_b)
{
	cpu_entry_t *a, *b;
	a = _a->value;
	b = _b->value;
	/* Note that a and b are inverted: we want the lowest-priority CPU at
	 * the top of the heap.
	 */
	return edf_higher_prio(b->linked, a->linked);
}

/* update_cpu_position - Move the cpu entry to the correct place to maintain
 *                       order in the cpu queue. Caller must hold gsnedf lock.
 */
static void update_cpu_position(cpu_entry_t *entry)
{
	if (likely(bheap_node_in_heap(entry->hn)))
		bheap_delete(cpu_lower_prio, &gsnedfm_cpu_heap, entry->hn);
	bheap_insert(cpu_lower_prio, &gsnedfm_cpu_heap, entry->hn);
}

/* caller must hold gsnedf lock */
static cpu_entry_t* lowest_prio_cpu(void)
{
	struct bheap_node* hn;
	hn = bheap_peek(cpu_lower_prio, &gsnedfm_cpu_heap);
	return hn->value;
}


/* link_task_to_cpu - Update the link of a CPU.
 *                    Handles the case where the to-be-linked task is already
 *                    scheduled on a different CPU.
 */
static noinline void link_task_to_cpu(struct task_struct* linked,
				      cpu_entry_t *entry)
{
	cpu_entry_t *sched;
	struct task_struct* tmp;
	int on_cpu;

	BUG_ON(linked && !is_realtime(linked));

	/* Currently linked task is set to be unlinked. */
	if (entry->linked) {
		entry->linked->rt_param.linked_on = NO_CPU;
	}

	/* Link new task to CPU. */
	if (linked) {
		/* handle task is already scheduled somewhere! */
		on_cpu = linked->rt_param.scheduled_on;
		if (on_cpu != NO_CPU) {
			sched = &per_cpu(gsnedfm_cpu_entries, on_cpu);
			/* this should only happen if not linked already */
			BUG_ON(sched->linked == linked);

			/* If we are already scheduled on the CPU to which we
			 * wanted to link, we don't need to do the swap --
			 * we just link ourselves to the CPU and depend on
			 * the caller to get things right.
			 */
			if (entry != sched) {
				TRACE_TASK(linked,
					   "already scheduled on %d, updating link.\n",
					   sched->cpu);
				tmp = sched->linked;
				linked->rt_param.linked_on = sched->cpu;
				sched->linked = linked;
				update_cpu_position(sched);
				linked = tmp;
			}
		}
		if (linked) /* might be NULL due to swap */
			linked->rt_param.linked_on = entry->cpu;
	}
	entry->linked = linked;
#ifdef WANT_ALL_SCHED_EVENTS
	if (linked)
		TRACE_TASK(linked, "linked to %d.\n", entry->cpu);
	else
		TRACE("NULL linked to %d.\n", entry->cpu);
#endif
	update_cpu_position(entry);
}

/* unlink - Make sure a task is not linked any longer to an entry
 *          where it was linked before. Must hold gsnedf_lock.
 */
static noinline void unlink(struct task_struct* t)
{
	cpu_entry_t *entry;
//	clean_budget(t->rt_param.linked_on);
	if (t->rt_param.linked_on != NO_CPU) {
		/* unlink */
		entry = &per_cpu(gsnedfm_cpu_entries, t->rt_param.linked_on);
		t->rt_param.linked_on = NO_CPU;
		link_task_to_cpu(NULL, entry);
	} else if (is_queued(t)) {
		/* This is an interesting situation: t is scheduled,
		 * but was just recently unlinked.  It cannot be
		 * linked anywhere else (because then it would have
		 * been relinked to this CPU), thus it must be in some
		 * queue. We must remove it from the list in this
		 * case.
		 */
		remove(&gsnedfm, t);
	}
	
}


/* preempt - force a CPU to reschedule
 */
static void preempt(cpu_entry_t *entry)
{
	preempt_if_preemptable(entry->scheduled, entry->cpu);
}

/* requeue - Put an unlinked task into gsn-edf domain.
 *           Caller must hold gsnedf_lock.
 */
static noinline void requeue(struct task_struct* task)
{
	BUG_ON(!task);
	/* sanity check before insertion */
	BUG_ON(is_queued(task));

	if (is_early_releasing(task) || is_released(task, litmus_clock()))
		__add_ready(&gsnedfm, task);
	else {
		/* it has got to wait */
		add_release(&gsnedfm, task);
	}
}

#ifdef CONFIG_SCHED_CPU_AFFINITY
static cpu_entry_t* gsnedfm_get_nearest_available_cpu(cpu_entry_t *start)
{
	cpu_entry_t *affinity;

	get_nearest_available_cpu(affinity, start, gsnedfm_cpu_entries,
#ifdef CONFIG_RELEASE_MASTER
			gsnedfm.release_master,
#else
			NO_CPU,
#endif
			cpu_online_mask);

	return(affinity);
}
#endif

/* check for any necessary preemptions */
static void check_for_preemptions(void)
{
	struct task_struct *task;
	cpu_entry_t *last;
	struct rt_task task_params;
	int cur_budget;
#ifdef CONFIG_PREFER_LOCAL_LINKING
	cpu_entry_t *local;
	//get_edf();
	/* Before linking to other CPUs, check first whether the local CPU is
	 * idle. */
	local = this_cpu_ptr(&gsnedfm_cpu_entries);
	task  = __peek_ready(&gsnedfm);

	if (task && !local->linked
#ifdef CONFIG_RELEASE_MASTER
	    && likely(local->cpu != gsnedfm.release_master)
#endif
//	   && local->cpu!=get_master  
		) {
		task = __take_ready(&gsnedfm);
		TRACE_TASK(task, "linking to local CPU %d to avoid IPI\n", local->cpu);
		task_params =task->rt_param.task_params;
		sys_get_rt_task_param(task->pid,&task_params);	
		TRACE_TASK(task,"check preempt membudget==%d\n",task_params.mem_budget_task);
//		get_membudget(local->cpu,task_params.mem_budget_task);		
		
		cur_budget=get_cur_budget();
		TRACE_TASK(task, "get curbudget==%d\n", cur_budget);
//		get_edf(local);
		
//		if(task_params.mem_budget_task>cur_budget){

/*			smp_mb();
			get_membudget(local->cpu,task_params.mem_budget_task);
			TRACE_TASK(task,"task_budget%d>cur_budget%d\n"
				,task_params.mem_budget_task,local->cur_budget);*/
//			gsnedf_task_block(task);
//			 TRACE_TASK(task, "task budget%d>cur_budget%d\n"
//			,task_params.mem_budget_task, cur_budget);
//			__add_release(&gsnedf, task);
//			__add_ready(&gsnedf, task);

//		}else{
			get_membudget(local->cpu,task_params.mem_budget_task);
			smp_mb();			
			link_task_to_cpu(task, local);
			preempt(local);
		}

	
#endif

	for (last = lowest_prio_cpu();
	     edf_preemption_needed(&gsnedfm, last->linked);
	     last = lowest_prio_cpu()) {
//&& last->cpu!=get_master
		/* preemption necessary */
//		smp_call_function_single(last->cpu,__get_edf,NULL,0);
		task = __take_ready(&gsnedfm);
		TRACE("check_for_preemptions: attempting to link task %d to %d\n",
		      task->pid, last->cpu);

#ifdef CONFIG_SCHED_CPU_AFFINITY
		{
			cpu_entry_t *affinity =
					gsnedfm_get_nearest_available_cpu(
						&per_cpu(gsnedfm_cpu_entries, task_cpu(task)));
			if (affinity)
				last = affinity;
			else if (requeue_preempted_job(last->linked))
				requeue(last->linked);
		}
#else
		if (requeue_preempted_job(last->linked))
			requeue(last->linked);
#endif
		task_params =task->rt_param.task_params;
		sys_get_rt_task_param(task->pid,&task_params);	
		TRACE_TASK(task,"check preempt membudget==%d\n",task_params.mem_budget_task);
		
//		get_membudget(last->cpu,task_params.mem_budget_task);		

		cur_budget=get_cur_budget();
		TRACE_TASK(task, "get curbudget==%d\n", cur_budget);	
//		get_edf(last);
		
//		if(task_params.mem_budget_task>cur_budget){
//			gsnedf_task_block(task);
//		        TRACE_TASK(task, "task budget%d>cur_budget%d\n"
//			,task_params.mem_budget_task, cur_budget);				
//			__add_release(&gsnedf, task);
//			__add_ready(&gsnedf, task);	
/*			smp_mb();		
                        get_membudget(last->cpu,task_params.mem_budget_task);
                	TRACE_TASK(task,"task_budget%d>cur_budget%d\n"
				,task_params.mem_budget_task,last->cur_budget);*/

//		}else{
			get_membudget(last->cpu,task_params.mem_budget_task);		
			smp_mb();
                        link_task_to_cpu(task, last);
                        preempt(last);
                }

}

/* gsnedf_job_arrival: task is either resumed or released */
static noinline void gsnedfm_job_arrival(struct task_struct* task)
{
	BUG_ON(!task);

	requeue(task);
	check_for_preemptions();
}

static void gsnedfm_release_jobs(rt_domain_t* rt, struct bheap* tasks)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&gsnedfm_lock, flags);

	__merge_ready(rt, tasks);
	check_for_preemptions();

	raw_spin_unlock_irqrestore(&gsnedfm_lock, flags);
}

/* caller holds gsnedf_lock */
static noinline void curr_job_completion(int forced)
{
	struct task_struct *t = current;
	BUG_ON(!t);
//	clean_budget(smp_processor_id());
	sched_trace_task_completion(t, forced);

	TRACE_TASK(t, "job_completion(forced=%d).\n", forced);

	/* set flags */
	tsk_rt(t)->completed = 0;
	/* prepare for next period */
	prepare_for_next_period(t);
	if (is_early_releasing(t) || is_released(t, litmus_clock()))
		sched_trace_task_release(t);
	/* unlink */
	unlink(t);
	/* requeue
	 * But don't requeue a blocking task. */
	if (is_current_running())
		gsnedfm_job_arrival(t);
}

/* Getting schedule() right is a bit tricky. schedule() may not make any
 * assumptions on the state of the current task since it may be called for a
 * number of reasons. The reasons include a scheduler_tick() determined that it
 * was necessary, because sys_exit_np() was called, because some Linux
 * subsystem determined so, or even (in the worst case) because there is a bug
 * hidden somewhere. Thus, we must take extreme care to determine what the
 * current state is.
 *
 * The CPU could currently be scheduling a task (or not), be linked (or not).
 *
 * The following assertions for the scheduled task could hold:
 *
 *      - !is_running(scheduled)        // the job blocks
 *	- scheduled->timeslice == 0	// the job completed (forcefully)
 *	- is_completed()		// the job completed (by syscall)
 * 	- linked != scheduled		// we need to reschedule (for any reason)
 * 	- is_np(scheduled)		// rescheduling must be delayed,
 *					   sys_exit_np must be requested
 *
 * Any of these can occur together.
 */
static struct task_struct* gsnedfm_schedule(struct task_struct * prev)
{
	cpu_entry_t* entry = this_cpu_ptr(&gsnedfm_cpu_entries);
	int out_of_time, sleep, preempt, np, exists, blocks;
	struct task_struct* next = NULL;
	struct rt_task task_params;
#ifdef CONFIG_RELEASE_MASTER
	/* Bail out early if we are the release master.
	 * The release master never schedules any real-time tasks.
	 */
	if (unlikely(gsnedfm.release_master == entry->cpu)) {
		sched_state_task_picked();
		return NULL;
	}
#endif
	
	raw_spin_lock(&gsnedfm_lock);

	/* sanity checking */
	BUG_ON(entry->scheduled && entry->scheduled != prev);
	BUG_ON(entry->scheduled && !is_realtime(prev));
	BUG_ON(is_realtime(prev) && !entry->scheduled);
	
//	BUG_ON(entry->cpu==entry->mem_master);
	/* (0) Determine state */
	exists      = entry->scheduled != NULL;
	blocks      = exists && !is_current_running();
	out_of_time = exists && budget_enforced(entry->scheduled)
		&& budget_exhausted(entry->scheduled);
	np 	    = exists && is_np(entry->scheduled);
	sleep	    = exists && is_completed(entry->scheduled);
	preempt     = entry->scheduled != entry->linked;
	task_params = prev->rt_param.task_params;
//	runout	    = entry->cur_budget<task_params.mem_budget_task;
#ifdef WANT_ALL_SCHED_EVENTS
	TRACE_TASK(prev, "invoked gsnedf_schedule.\n");
#endif

	if (exists){
		sys_get_rt_task_param(prev->pid,&task_params);
		
		TRACE_TASK(prev,
			   "blocks:%d out_of_time:%d np:%d sleep:%d preempt:%d "
			   "state:%d sig:%d,membudget=%dMb/s\n",
			   blocks, out_of_time, np, sleep, preempt,
			   prev->state, signal_pending(prev),task_params.mem_budget_task);
/*		if(entry->cur_budget<entry->task_budget){
			TRACE_TASK(prev,"entry->cur_budget<task_budget\n");
			unlink(prev);
			gsnedf_job_arrival(prev);

		}
*/		
	}
	if (entry->linked && preempt)
		TRACE_TASK(prev, "will be preempted by %s/%d\n",
			   entry->linked->comm, entry->linked->pid);


	/* If a task blocks we have no choice but to reschedule.
	 */
	if (blocks)
		unlink(entry->scheduled);

	/* Request a sys_exit_np() call if we would like to preempt but cannot.
	 * We need to make sure to update the link structure anyway in case
	 * that we are still linked. Multiple calls to request_exit_np() don't
	 * hurt.
	 */
	if (np && (out_of_time || preempt || sleep)) {
		unlink(entry->scheduled);
		request_exit_np(entry->scheduled);
	}

	/* Any task that is preemptable and either exhausts its execution
	 * budget or wants to sleep completes. We may have to reschedule after
	 * this. Don't do a job completion if we block (can't have timers running
	 * for blocked jobs).
	 */
	if (!np && (out_of_time || sleep))
		curr_job_completion(!sleep);

	/* Link pending task if we became unlinked.
	 */
	if (!entry->linked)
		link_task_to_cpu(__take_ready(&gsnedfm), entry);

	/* The final scheduling decision. Do we need to switch for some reason?
	 * If linked is different from scheduled, then select linked as next.
	 */
	if ((!np || blocks) &&
	    entry->linked != entry->scheduled) {
		/* Schedule a linked job? */
		if (entry->linked) {
			entry->linked->rt_param.scheduled_on = entry->cpu;
			next = entry->linked;
			TRACE_TASK(next, "scheduled_on = P%d\n", smp_processor_id());
		}
		if (entry->scheduled) {
			/* not gonna be scheduled soon */
			entry->scheduled->rt_param.scheduled_on = NO_CPU;
			TRACE_TASK(entry->scheduled, "scheduled_on = NO_CPU\n");
		}
	} else
		/* Only override Linux scheduler if we have a real-time task
		 * scheduled that needs to continue.
		 */
		if (exists)
			next = prev;

	sched_state_task_picked();

	raw_spin_unlock(&gsnedfm_lock);

#ifdef WANT_ALL_SCHED_EVENTS
	TRACE("gsnedfm_lock released, next=0x%p\n", next);

	if (next)
		TRACE_TASK(next, "scheduled at %llu\n", litmus_clock());
	else if (exists && !next)
		TRACE("becomes idle at %llu.\n", litmus_clock());
#endif


	return next;
}


/* _finish_switch - we just finished the switch away from prev
 */
static void gsnedfm_finish_switch(struct task_struct *prev)
{
	cpu_entry_t* 	entry = this_cpu_ptr(&gsnedfm_cpu_entries);

	entry->scheduled = is_realtime(current) ? current : NULL;
#ifdef WANT_ALL_SCHED_EVENTS
	TRACE_TASK(prev, "switched away from\n");
#endif
}


/*	Prepare a task for running in RT mode
 */
static void gsnedfm_task_new(struct task_struct * t, int on_rq, int is_scheduled)
{
	unsigned long 		flags;
	cpu_entry_t* 		entry;

	TRACE("gsn edf m: task new %d\n", t->pid);

	raw_spin_lock_irqsave(&gsnedfm_lock, flags);

	/* setup job params */
	release_at(t, litmus_clock());

	if (is_scheduled) {
		entry = &per_cpu(gsnedfm_cpu_entries, task_cpu(t));
		BUG_ON(entry->scheduled);

#ifdef CONFIG_RELEASE_MASTER
		if (entry->cpu != gsnedfm.release_master) {
#endif
			entry->scheduled = t;
			tsk_rt(t)->scheduled_on = task_cpu(t);
#ifdef CONFIG_RELEASE_MASTER
		} else {
			/* do not schedule on release master */
			preempt(entry); /* force resched */
			tsk_rt(t)->scheduled_on = NO_CPU;
		}
#endif
	} else {
		t->rt_param.scheduled_on = NO_CPU;
	}
	t->rt_param.linked_on          = NO_CPU;

	if (on_rq || is_scheduled)
		gsnedfm_job_arrival(t);
	raw_spin_unlock_irqrestore(&gsnedfm_lock, flags);
}

static void gsnedfm_task_wake_up(struct task_struct *task)
{
	unsigned long flags;
	lt_t now;

	TRACE_TASK(task, "wake_up at %llu\n", litmus_clock());

	raw_spin_lock_irqsave(&gsnedfm_lock, flags);
	now = litmus_clock();
	if (is_sporadic(task) && is_tardy(task, now)) {
		inferred_sporadic_job_release_at(task, now);
	}
	gsnedfm_job_arrival(task);
	raw_spin_unlock_irqrestore(&gsnedfm_lock, flags);
}

static void gsnedfm_task_block(struct task_struct *t)
{
	unsigned long flags;

	TRACE_TASK(t, "block at %llu\n", litmus_clock());

	/* unlink if necessary */
	raw_spin_lock_irqsave(&gsnedfm_lock, flags);
	unlink(t);
	raw_spin_unlock_irqrestore(&gsnedfm_lock, flags);

	BUG_ON(!is_realtime(t));
}


static void gsnedfm_task_exit(struct task_struct * t)
{
	unsigned long flags;
//	clean_budget(smp_processor_id());
	/* unlink if necessary */
	raw_spin_lock_irqsave(&gsnedfm_lock, flags);
	unlink(t);
	if (tsk_rt(t)->scheduled_on != NO_CPU) {
		gsnedfm_cpus[tsk_rt(t)->scheduled_on]->scheduled = NULL;
		tsk_rt(t)->scheduled_on = NO_CPU;
	}

	raw_spin_unlock_irqrestore(&gsnedfm_lock, flags);

	BUG_ON(!is_realtime(t));
        TRACE_TASK(t, "RIP\n");
}


static long gsnedfm_admit_task(struct task_struct* tsk)
{
	return 0;
}



/* called with IRQs off */
static void set_priority_inheritance(struct task_struct* t, struct task_struct* prio_inh)
{
	int linked_on;
	int check_preempt = 0;

	raw_spin_lock(&gsnedfm_lock);

	TRACE_TASK(t, "inherits priority from %s/%d\n", prio_inh->comm, prio_inh->pid);
	tsk_rt(t)->inh_task = prio_inh;

	linked_on  = tsk_rt(t)->linked_on;

	/* If it is scheduled, then we need to reorder the CPU heap. */
	if (linked_on != NO_CPU) {
		TRACE_TASK(t, "%s: linked  on %d\n",
			   __FUNCTION__, linked_on);
		/* Holder is scheduled; need to re-order CPUs.
		 * We can't use heap_decrease() here since
		 * the cpu_heap is ordered in reverse direction, so
		 * it is actually an increase. */
		bheap_delete(cpu_lower_prio, &gsnedfm_cpu_heap,
			    gsnedfm_cpus[linked_on]->hn);
		bheap_insert(cpu_lower_prio, &gsnedfm_cpu_heap,
			    gsnedfm_cpus[linked_on]->hn);
	} else {
		/* holder may be queued: first stop queue changes */
		raw_spin_lock(&gsnedfm.release_lock);
		if (is_queued(t)) {
			TRACE_TASK(t, "%s: is queued\n",
				   __FUNCTION__);
			/* We need to update the position of holder in some
			 * heap. Note that this could be a release heap if we
			 * budget enforcement is used and this job overran. */
			check_preempt =
				!bheap_decrease(edf_ready_order,
					       tsk_rt(t)->heap_node);
		} else {
			/* Nothing to do: if it is not queued and not linked
			 * then it is either sleeping or currently being moved
			 * by other code (e.g., a timer interrupt handler) that
			 * will use the correct priority when enqueuing the
			 * task. */
			TRACE_TASK(t, "%s: is NOT queued => Done.\n",
				   __FUNCTION__);
		}
		raw_spin_unlock(&gsnedfm.release_lock);

		/* If holder was enqueued in a release heap, then the following
		 * preemption check is pointless, but we can't easily detect
		 * that case. If you want to fix this, then consider that
		 * simply adding a state flag requires O(n) time to update when
		 * releasing n tasks, which conflicts with the goal to have
		 * O(log n) merges. */
		if (check_preempt) {
			/* heap_decrease() hit the top level of the heap: make
			 * sure preemption checks get the right task, not the
			 * potentially stale cache. */
			bheap_uncache_min(edf_ready_order,
					 &gsnedfm.ready_queue);
			check_for_preemptions();
		}
	}

	raw_spin_unlock(&gsnedfm_lock);
}

/* called with IRQs off */
static void clear_priority_inheritance(struct task_struct* t)
{
	raw_spin_lock(&gsnedfm_lock);

	/* A job only stops inheriting a priority when it releases a
	 * resource. Thus we can make the following assumption.*/
	BUG_ON(tsk_rt(t)->scheduled_on == NO_CPU);

	TRACE_TASK(t, "priority restored\n");
	tsk_rt(t)->inh_task = NULL;

	/* Check if rescheduling is necessary. We can't use heap_decrease()
	 * since the priority was effectively lowered. */
	unlink(t);
	gsnedfm_job_arrival(t);

	raw_spin_unlock(&gsnedfm_lock);
}



static struct domain_proc_info gsnedfm_domain_proc_info;
static long gsnedfm_get_domain_proc_info(struct domain_proc_info **ret)
{
	*ret = &gsnedfm_domain_proc_info;
	return 0;
}

static void gsnedfm_setup_domain_proc(void)
{
	int i, cpu;
	int release_master =
#ifdef CONFIG_RELEASE_MASTER
			atomic_read(&release_master_cpu);
#else
		NO_CPU;
#endif
	int num_rt_cpus = num_online_cpus() - (release_master != NO_CPU);
	struct cd_mapping *map;

	memset(&gsnedfm_domain_proc_info, 0, sizeof(gsnedfm_domain_proc_info));
	init_domain_proc_info(&gsnedfm_domain_proc_info, num_rt_cpus, 1);
	gsnedfm_domain_proc_info.num_cpus = num_rt_cpus;
	gsnedfm_domain_proc_info.num_domains = 1;

	gsnedfm_domain_proc_info.domain_to_cpus[0].id = 0;
	for (cpu = 0, i = 0; cpu < num_online_cpus(); ++cpu) {
		if (cpu == release_master)
			continue;
		map = &gsnedfm_domain_proc_info.cpu_to_domains[i];
		map->id = cpu;
		cpumask_set_cpu(0, map->mask);
		++i;

		/* add cpu to the domain */
		cpumask_set_cpu(cpu,
			gsnedfm_domain_proc_info.domain_to_cpus[0].mask);
	}
}

static long gsnedfm_activate_plugin(void)
{
	int cpu;
	cpu_entry_t *entry;

	bheap_init(&gsnedfm_cpu_heap);
#ifdef CONFIG_RELEASE_MASTER
	gsnedfm.release_master = atomic_read(&release_master_cpu);
#endif

	for_each_online_cpu(cpu) {
		entry = &per_cpu(gsnedfm_cpu_entries, cpu);
		bheap_node_init(&entry->hn, entry);
		entry->linked    = NULL;
		entry->scheduled = NULL;
#ifdef CONFIG_RELEASE_MASTER
		if (cpu != gsnedfm.release_master) {
			TRACE("gsnedfm.release_master==%d.\n", gsnedfm.release_master);
#endif
			TRACE("GSN-EDFM: Initializing CPU #%d.\n", cpu);
			update_cpu_position(entry);
#ifdef CONFIG_RELEASE_MASTER
		} else {
			TRACE("GSN-EDFM: CPU %d is release master.\n", cpu);
		}
#endif
	}

	gsnedfm_setup_domain_proc();

	return 0;
}

static long gsnedfm_deactivate_plugin(void)
{
	destroy_domain_proc_info(&gsnedfm_domain_proc_info);
	return 0;
}

/*	Plugin object	*/
static struct sched_plugin gsn_edfm_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "GSN-EDFM",
	.finish_switch		= gsnedfm_finish_switch,
	.task_new		= gsnedfm_task_new,
	.complete_job		= complete_job,
	.task_exit		= gsnedfm_task_exit,
	.schedule		= gsnedfm_schedule,
	.task_wake_up		= gsnedfm_task_wake_up,
	.task_block		= gsnedfm_task_block,
	.admit_task		= gsnedfm_admit_task,
	.activate_plugin	= gsnedfm_activate_plugin,
	.deactivate_plugin	= gsnedfm_deactivate_plugin,
	.get_domain_proc_info	= gsnedfm_get_domain_proc_info,
//#ifdef CONFIG_LITMUS_LOCKING
//	.allocate_lock		= gsnedfm_allocate_lock,
//#endif
};


static int __init init_gsn_edfm(void)
{
	int cpu;
	cpu_entry_t *entry;

	bheap_init(&gsnedfm_cpu_heap);
	/* initialize CPU state */
	for (cpu = 0; cpu < NR_CPUS; cpu++)  {
		entry = &per_cpu(gsnedfm_cpu_entries, cpu);
		gsnedfm_cpus[cpu] = entry;
		entry->cpu 	 = cpu;
		entry->hn        = &gsnedfm_heap_node[cpu];
		bheap_node_init(&entry->hn, entry);
//		entry->cur_budget= 360;
		
	}

	edf_domain_init(&gsnedfm, NULL, gsnedfm_release_jobs);
	return register_sched_plugin(&gsn_edfm_plugin);
}

//EXPORT_SYMBOL(get_edfbudget);
module_init(init_gsn_edfm);
