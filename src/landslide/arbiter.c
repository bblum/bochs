/**
 * @file arbiter.c
 * @author Ben Blum
 * @brief decision-making routines for landslide
 *
 * Copyright (c) 2018, Ben Blum
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>

#define MODULE_NAME "ARBITER"
#define MODULE_COLOUR COLOUR_YELLOW

#include "common.h"
#include "found_a_bug.h"
#include "kernel_specifics.h"
#include "kspec.h"
#include "landslide.h"
#include "mem.h"
#include "pp.h"
#include "rand.h"
#include "schedule.h"
#include "tsx.h"
#include "user_specifics.h"
#include "user_sync.h"
#include "x86.h"

void arbiter_init(struct arbiter_state *r)
{
	Q_INIT_HEAD(&r->choices);
}

void arbiter_append_choice(struct arbiter_state *r, unsigned int tid, bool txn, unsigned int xabort_code, struct abort_set *aborts)
{
	struct choice *c = MM_XMALLOC(1, struct choice);
	c->tid = tid;
	c->txn = txn;
	c->xabort_code = xabort_code;
	c->aborts = *aborts;
	Q_INSERT_FRONT(&r->choices, c, nobe);
}

bool arbiter_pop_choice(struct arbiter_state *r, unsigned int *tid, bool *txn, unsigned int *xabort_code, struct abort_set *aborts)
{
	struct choice *c = Q_GET_TAIL(&r->choices);
	if (c) {
		lsprintf(DEV, "using requested tid %d\n", c->tid);
		Q_REMOVE(&r->choices, c, nobe);
		*tid = c->tid;
		*txn = c->txn;
		*xabort_code = c->xabort_code;
		*aborts = c->aborts;
		MM_FREE(c);
		return true;
	} else {
		return false;
	}
}

#define ASSERT_ONE_THREAD_PER_PP(ls) do {					\
		assert((/* root pp not created yet */				\
		        (ls)->save.next_tid == TID_NONE ||			\
		        /* thread that was chosen is still running */		\
		        (ls)->save.next_tid == (ls)->sched.cur_agent->tid) &&	\
		       "One thread per preemption point invariant violated!");	\
	} while (0)

bool arbiter_interested(struct ls_state *ls, bool just_finished_reschedule,
			bool *voluntary, bool *need_handle_sleep, bool *data_race,
			bool *joined, bool *xbegin)
{
	*voluntary = false;
	*need_handle_sleep = false;
	*data_race = false;
	*joined = false;
	*xbegin = false;

	/* Attempt to see if a "voluntary" reschedule is just ending - did the
	 * last thread context switch not because of a timer?
	 * Also make sure to ignore null switches (timer-driven or not). */
	if (ls->sched.last_agent != NULL &&
	    !ls->sched.last_agent->action.handling_timer &&
	    ls->sched.last_agent != ls->sched.cur_agent &&
	    just_finished_reschedule) {
		lsprintf(DEV, "a voluntary reschedule: ");
		print_agent(DEV, ls->sched.last_agent);
		printf(DEV, " to ");
		print_agent(DEV, ls->sched.cur_agent);
		printf(DEV, "\n");
#ifndef PINTOS_KERNEL
		/* Pintos includes a semaphore implementation which can go
		 * around its anti-paradise-lost while loop a full time without
		 * interrupts coming back on. So, there can be a voluntary
		 * reschedule sequence where an uninterruptible, blocked thread
		 * gets jammed in the middle of this transition. Issue #165. */
		if (ls->save.next_tid != ls->sched.last_agent->tid) {
			ASSERT_ONE_THREAD_PER_PP(ls);
		}
#endif
		assert(ls->sched.voluntary_resched_tid != TID_NONE);
		*voluntary = true;
		return true;
	/* is the kernel idling, e.g. waiting for keyboard input? */
	} else if (ls->instruction_text[0] == OPCODE_HLT) {
		lskprintf(INFO, "What are you waiting for? (HLT state)\n");
		*need_handle_sleep = true;
		ASSERT_ONE_THREAD_PER_PP(ls);
		return true;
	/* Skip the instructions before the test case itself gets started. In
	 * many kernels' cases this will be redundant, but just in case. */
	} else if (!ls->test.test_ever_caused ||
		   ls->test.start_population == ls->sched.most_agents_ever) {
		return false;
	/* check for data races */
	} else if (suspected_data_race(ls)
		   /* if xchg-blocked, need NOT set DR PP. other case below. */
		   && !XCHG_BLOCKED(&ls->sched.cur_agent->user_yield)
#ifdef DR_PPS_RESPECT_WITHIN_FUNCTIONS
		   // NB. The use of KERNEL_MEMORY here used to be !testing_userspace.
		   // I needed to change it to implement preempt-everywhere mode,
		   // to handle the case of userspace shms in deschedule() syscall.
		   // Not entirely sure of all implications of this change.
		   && ((!KERNEL_MEMORY(ls->eip) && user_within_functions(ls)) ||
		      (KERNEL_MEMORY(ls->eip) && kern_within_functions(ls)))
#endif
#ifndef HTM_WEAK_ATOMICITY
		   && !ls->sched.cur_agent->action.user_txn
#endif
		   ) {
		*data_race = true;
		ASSERT_ONE_THREAD_PER_PP(ls);
		return true;
	/* user-mode-only preemption points */
	} else if (testing_userspace()) {
		unsigned int mutex_addr;
		if (KERNEL_MEMORY(ls->eip)) {
#ifdef GUEST_YIELD_ENTER
#ifndef GUEST_YIELD_EXIT
			STATIC_ASSERT(false && "missing guest yield exit");
#endif
			if ((ls->eip == GUEST_YIELD_ENTER &&
			     READ_STACK(ls->cpu0, 1) == ls->sched.cur_agent->tid) ||
			    (ls->eip == GUEST_YIELD_EXIT &&
			     ((signed int)GET_CPU_ATTR(ls->cpu0, eax)) < 0)) {
				/* Busted yield. Pretend it was yield -1. */
				ASSERT_ONE_THREAD_PER_PP(ls);
				return true;
			}
#endif
			return false;
		} else if (XCHG_BLOCKED(&ls->sched.cur_agent->user_yield)) {
			/* User thread is blocked on an "xchg-continue" mutex.
			 * Analogous to HLT state -- need to preempt it. */
			ASSERT_ONE_THREAD_PER_PP(ls);
#ifndef HTM_WEAK_ATOMICITY
			/* under strong atomicity, if for whatever reason a txn
			 * blocks, there's no way it should ever succeed */
			if (ls->sched.cur_agent->action.user_txn) {
				abort_transaction(ls->sched.cur_agent->tid,
						  ls->save.current, _XABORT_CAPACITY);
				ls->end_branch_early = true;
				return false;
			}
#endif
			return true;
#ifndef PINTOS_KERNEL
		} else if (!check_user_address_space(ls)) {
			return false;
#endif
		} else if ((user_mutex_lock_entering(ls->cpu0, ls->eip, &mutex_addr) ||
			    user_mutex_unlock_exiting(ls->eip)) &&
			   user_within_functions(ls)) {
			ASSERT_ONE_THREAD_PER_PP(ls);
#ifndef HTM_WEAK_ATOMICITY
			/* by the equivalence proof, it's sound to skip this pp
			 * because if anything were to conflict with it, it'd be
			 * the same as if the txn aborted to begin with */
			if (ls->sched.cur_agent->action.user_txn) {
				return false;
			}
			/* on other hand, under weak memory maybe the user needs
			 * this mutex to protect against some non-txnal code */
#endif
			return true;
#ifdef USER_MAKE_RUNNABLE_EXIT
		} else if (ls->eip == USER_MAKE_RUNNABLE_EXIT) {
			/* i think the reference kernel version i have might
			 * predate the make runnable misbehave mode, because it
			 * seems not to be putting yield pps on it.*/
			ASSERT_ONE_THREAD_PER_PP(ls);
			return true;
#endif
#ifdef TRUSTED_THR_JOIN
		} else if (user_thr_join_exiting(ls->eip)) {
			/* don't respect within functions, obv; this pp is for
			 * happens-before purposes, not scheduling, anyway */
			ASSERT_ONE_THREAD_PER_PP(ls);
			*joined = true;
			return true;
#ifndef USER_MAKE_RUNNABLE_EXIT
		} else if (true) {
			assert(0 && "need mkrun pp for trusted join soundness");
#endif
#endif
		} else if (user_xbegin_entering(ls->eip) ||
			   user_xend_entering(ls->eip)) {
			/* Have to disrespect within functions to properly
			 * respect htm-blocking if there's contention. */
			ASSERT_ONE_THREAD_PER_PP(ls);
			*xbegin = user_xbegin_entering(ls->eip);
			return true;
		} else {
			return false;
		}
	/* kernel-mode-only preemption points */
#ifdef PINTOS_KERNEL
	} else if ((ls->eip == GUEST_SEMA_DOWN_ENTER || ls->eip == GUEST_SEMA_UP_EXIT) && kern_within_functions(ls)) {
		ASSERT_ONE_THREAD_PER_PP(ls);
		return true;
	} else if ((ls->eip == GUEST_CLI_ENTER || ls->eip == GUEST_STI_EXIT) &&
		   !ls->sched.cur_agent->action.kern_mutex_locking &&
		   !ls->sched.cur_agent->action.kern_mutex_unlocking &&
		   kern_within_functions(ls)) {
		ASSERT_ONE_THREAD_PER_PP(ls);
		return true;
#endif
	} else if (kern_decision_point(ls->eip) &&
		   kern_within_functions(ls)) {
		ASSERT_ONE_THREAD_PER_PP(ls);
		return true;
	} else {
		return false;
	}
}

static bool report_deadlock(struct ls_state *ls)
{
	if (BUG_ON_THREADS_WEDGED == 0) {
		return false;
	}

	if (!anybody_alive(ls->cpu0, &ls->test, &ls->sched, true)) {
		/* No threads exist. Not a deadlock, but rather end of test. */
		return false;
	}

	struct agent *a;
	FOR_EACH_RUNNABLE_AGENT(a, &ls->sched,
		if (BLOCKED(a) && a->action.disk_io) {
			lsprintf(CHOICE, COLOUR_BOLD COLOUR_YELLOW "Warning, "
				 "'ad-hoc' yield blocking (mutexes?) is not "
				 "suitable for disk I/O! (TID %d)\n", a->tid);
			return false;
		}
	);
	/* Now do for each *non*-runnable agent... */
	Q_FOREACH(a, &ls->sched.dq, nobe) {
		if (a->action.disk_io) {
			lsprintf(CHOICE, "TID %d blocked on disk I/O. "
				 "Allowing idle to run.\n", a->tid);
			return false;
		}
	}
	return true;
}

#define IS_IDLE(ls, a)							\
	(TID_IS_IDLE((a)->tid) &&					\
	 BUG_ON_THREADS_WEDGED != 0 && (ls)->test.test_ever_caused &&	\
	 (ls)->test.start_population != (ls)->sched.most_agents_ever)

/* Attempting to track whether threads are "blocked" based on when they call
 * yield() while inside mutex_lock() is great for avoiding the expensive
 * yield-loop-counting heuristic, it can produce some false positive deadlocks
 * when a thread's blocked-on-addr doesn't get unset at the right time. A good
 * example is when mutex_lock actually deschedule()s, and has a little-lock
 * inside that yields. We can't know (without annotations) that we need to
 * unset contenders' blocked-on-addrs when e.g. little_lock_unlock() is called
 * at the end of mutex_lock().
 *
 * The tradeoff with this knob is how long FAB traces are for deadlock reports,
 * versus how many benign repetitions an adversarial program must contain in
 * order to trigger a false positive report despite this cleverness. */
#define DEADLOCK_FP_MAX_ATTEMPTS 128
static bool try_avoid_fp_deadlock(struct ls_state *ls, bool voluntary,
				  struct agent **result) {
	/* The counter is reset every time we backtrack, but it's never reset
	 * during a single branch. This gives some notion of progress, so we
	 * won't just try this strategy forever in a real deadlock situation. */
	if (ls->sched.deadlock_fp_avoidance_count == DEADLOCK_FP_MAX_ATTEMPTS) {
		return false;
	}

	ls->sched.deadlock_fp_avoidance_count++;

	bool found_one = false;
	struct agent *a;

	/* We must prioritize trying ICB-blocked threads higher than yield/xchg-
	 * blocked ones, because ICB-blocked threads won't get run "on their
	 * own" at subsequent PPs; rather we must force it immediately here.
	 * In fact, we must check *all* threads for being ICB-blocked before
	 * checking *any* for other kinds of blockage, so that we don't awaken
	 * the latter type unnecessarily (resulting in infinite subtrees). */
	FOR_EACH_RUNNABLE_AGENT(a, &ls->sched,
		if (ICB_BLOCKED(&ls->sched, ls->icb_bound, voluntary, a)) {
			assert(!IS_IDLE(ls, a) && "That's weird.");
			/* a thread could be multiple types of maybe-blocked at
			 * once. skip those for now; prioritizing ICB-blocked
			 * ones that are definitely otherwise runnable. */
			if (a->user_blocked_on_addr == ADDR_NONE &&
			    !agent_is_user_yield_blocked(&a->user_yield)) {
				lsprintf(DEV, "I thought TID %d was ICB-blocked "
					 "(bound %u), but maybe preempting is "
					 "needed here for  correctness!\n",
					 a->tid, ls->icb_bound);
				*result = a;
				found_one = true;
			}
		}
	);

	if (found_one) {
		/* Found ICB-blocked thread to wake. Return early. */
		return found_one;
	}

#ifdef HTM_ABORT_SETS
	/* check for false positive abort set blocking -- it takes until
	 * htm2(3,2) 900K+ interleavings to first trip this but it's real!
	 * this doesn't appear to affect SS size in any non-deadlocking tests,
	 * but in case it does (fp deadlock avoid for other reasons?) you might
	 * need to have two "phases" of fp deadlock detection. of course this
	 * can't go after the following part, because it needs to have higher
	 * priority than an actual mutex bc otherwise the (actually blocked)
	 * mutex-blocked thread would just "consume" all the attempts */
	FOR_EACH_RUNNABLE_AGENT(a, &ls->sched,
		struct abort_set *aborts = &ls->sched.upcoming_aborts;
		if (ABORT_SET_BLOCKED(aborts, a->tid)) {
			lsprintf(BRANCH, "I thought TID %d was abort-set "
				 "blocked, but I could be wrong!\n", a->tid);
			/* unblock the to-execute-later tid and let it run,
			 * giving up on the reduction */
			aborts->preempted_evil_ancestor.tid = TID_NONE;
			/* FIXME: not sure if even possible to mark the abort
			 * set "abandoned" in the original nobe it came from?
			 * bc we might be deep in its subtree, and other parts
			 * of the subtree still want to apply the reduction. */
			*result = a;
			found_one = true;
		}
	);
	if (found_one) {
		return found_one;
	}
#endif

	/* Doesn't matter which thread we choose; take whichever is latest in
	 * this loop. But we need to wake all of them, not knowing which was
	 * "faking it". If it's truly deadlocked, they'll all block again. */
	FOR_EACH_RUNNABLE_AGENT(a, &ls->sched,
		if (a->user_blocked_on_addr != ADDR_NONE) {
			assert(!IS_IDLE(ls, a) && "That's weird.");
			lsprintf(DEV, "I thought TID %d was blocked on 0x%x, "
				 "but I could be wrong!\n",
				 a->tid, a->user_blocked_on_addr);
			a->user_blocked_on_addr = ADDR_NONE;
			*result = a;
			found_one = true;
		} else if (agent_is_user_yield_blocked(&a->user_yield)) {
			assert(!IS_IDLE(ls, a) && "That's weird.");
			lsprintf(DEV, "I thought TID %d was blocked yielding "
				 "(ylc %u), but I could be wrong!\n",
				 a->tid, a->user_yield.loop_count);
			a->user_yield.loop_count = 0;
			a->user_yield.blocked = false;
			*result = a;
			found_one = true;
		}
	);
	return found_one;
}

/* this improves state space reduction (it's basically the other half of
 * 'sleep sets', that equiv-already-explored covers the other half of).
 * whenever dpor tells scheduler to switch to a particular tid, that tid should
 * be treated as higher priority to run than whatever was preempted. */
#define KEEP_RUNNING_DPORS_CHOSEN_TID
/* should we remember every thread dpor's chosen to preempt to in this branch's
 * history, or only the latest one? e.g if dpor put us in a subtree by switching
 * to thread 5, then into a further subtree of that (by backtracking a shorter
 * distance) by switching to thread 6, then when thread 6 blocks on something,
 * should we let the scheduler randomly switch to thread 4, or fall back on a
 * preference for thread 5?
 * remembering every priority causes state space reduction in some cases
 * (htm_fig63(3,1)), but also inflation in other cases (swap(3,1)), and the
 * inflation is generally worse, so it's disabled by default. i have no evidence
 * of it affecting SS size with only 2 threads either way though. */
#define CONSIDER_ONLY_MOST_RECENT_DPOR_PREFERRED_TID

/* Returns true if a thread was chosen. If true, sets 'target' (to either the
 * current thread or any other thread), and sets 'our_choice' to false if
 * somebody else already made this choice for us, true otherwise. */
bool arbiter_choose(struct ls_state *ls, struct agent *current, bool voluntary,
		    struct agent **result, bool *our_choice)
{
	struct agent *a;
	unsigned int count = 0;
	bool current_is_legal_choice = false;
	bool dpor_preferred_is_legal_choice = false;
	unsigned int dpor_preferred_count;
	unsigned int dpor_preference = 0;

	/* We shouldn't be asked to choose if somebody else already did. */
	assert(Q_GET_SIZE(&ls->arbiter.choices) == 0);

	lsprintf(DEV, "Available choices: ");

	/* Count the number of available threads. */
	FOR_EACH_RUNNABLE_AGENT(a, &ls->sched,
		if (!BLOCKED(a) && !IS_IDLE(ls, a) &&
		    !HTM_BLOCKED(&ls->sched, a) &&
		    !ABORT_SET_BLOCKED(&ls->sched.upcoming_aborts, a->tid) &&
		    !ICB_BLOCKED(&ls->sched, ls->icb_bound, voluntary, a)) {
			print_agent(DEV, a);
			printf(DEV, " ");
			count++;
			if (a == current) {
				current_is_legal_choice = true;
			}
#ifdef KEEP_RUNNING_DPORS_CHOSEN_TID
			/* i don't remember which test case it was that made me
			 * keep a stack of preferred tids instead of just the
			 * latest one, and i think the trusted-join stuff might
			 * subsume any marginal benefit the stack gives, but,
			 * the stack still seems right in principle. vOv */
			unsigned int i;
			unsigned int *dpor_preferred_tid;
			/* consider all tids which dpor has chosen to switch to
			 * so far in this branch, with preference for latest */
			ARRAY_LIST_FOREACH(&ls->sched.dpor_preferred_tids, i,
					   dpor_preferred_tid) {
#ifdef CONSIDER_ONLY_MOST_RECENT_DPOR_PREFERRED_TID
				/* actually, consider only the most recent */
				if (i < ARRAY_LIST_SIZE(
					&ls->sched.dpor_preferred_tids) - 1) {
					continue;
				}
#endif
				if (a->tid == *dpor_preferred_tid &&
				    i >= dpor_preference) {
					dpor_preferred_is_legal_choice = true;
					dpor_preferred_count = count;
					dpor_preference = i;
				}
			}
#endif
		}
	);

//#define CHOOSE_RANDOMLY
#ifdef CHOOSE_RANDOMLY
#ifdef ICB
	STATIC_ASSERT(false && "ICB and CHOOSE_RANDOMLY are incompatible");
#endif
	STATIC_ASSERT(false && "TODO: find a bsd random number generator");
	// with given odds, will make the "forwards" choice.
	const int numerator   = 19;
	const int denominator = 20;
	if (rand64(&ls->rand) % denominator < numerator) {
		count = 1;
	}
#else
	if (EXPLORE_BACKWARDS == 0) {
		count = 1;
	} else {
#ifdef ICB
		assert(false && "For ICB, EXPLORE_BACKWARDS must be 0.");
#endif
	}
#endif
	if (dpor_preferred_is_legal_choice &&
	    // FIXME: i'm not sure if this is right, but seems to make no diff..
	    !current_is_legal_choice) {
		/* don't let voluntary context switches accidentally switch to
		 * the preempted evil ancestor before the child gets to run */
		count = dpor_preferred_count;
	}

	if (agent_has_yielded(&current->user_yield) ||
	    agent_has_xchged(&ls->user_sync)) {
		if (current_is_legal_choice) {
			printf(DEV, "- Must run yielding thread %d\n",
			       current->tid);
			/* NB. this will be last_agent when yielding. */
			*result = current;
			*our_choice = true;
			/* Preemption count doesn't increase. */
			return true;
		} else if (!agent_is_user_yield_blocked(&current->user_yield)) {
			/* Something funny happened, causing the thread to get
			 * ACTUALLY blocked before finishing yield-blocking. Any
			 * false-positive yield senario could trigger this. */
			assert(!current->user_yield.blocked);
			current->user_yield.loop_count = 0;
		} else {
			/* Normal case of blocking with TOO MANY YIELDS. */
		}
	}

	/* Find the count-th thread. */
	unsigned int i = 0;
	FOR_EACH_RUNNABLE_AGENT(a, &ls->sched,
		if (!BLOCKED(a) && !IS_IDLE(ls, a) &&
		    !HTM_BLOCKED(&ls->sched, a) &&
		    !ABORT_SET_BLOCKED(&ls->sched.upcoming_aborts, a->tid) &&
		    !ICB_BLOCKED(&ls->sched, ls->icb_bound, voluntary, a) &&
		    ++i == count) {
			printf(DEV, "- Figured I'd look at TID %d next.\n",
			       a->tid);
			*result = a;
			*our_choice = true;
			/* Should preemption counter increase for ICB? */
			// FIXME: actually, I'm pretty sure this is dead code.
			// Given EXPLORE_BACKWARDS=0, don't we always choose
			// either the cur agent or the last agent?
			if (!NO_PREEMPTION_REQUIRED(&ls->sched, voluntary, a)) {
				ls->sched.icb_preemption_count++;
				lsprintf(DEV, "Switching to TID %d will count "
					 "as a preemption for ICB.\n", a->tid);
			}
			return true;
		}
	);

	printf(DEV, "... none?\n");

	/* No runnable threads. Is this a bug, or is it expected? */
	if (report_deadlock(ls)) {
		if (try_avoid_fp_deadlock(ls, voluntary, result)) {
			lsprintf(CHOICE, COLOUR_BOLD COLOUR_YELLOW
				 "WARNING: System is apparently deadlocked! "
				 "Let me just try one thing. See you soon.\n");
			*our_choice = true;
			/* Special case. Bypass preemption count; this mechanism
			 * is needed for correctness, so ICB can't interfere. */
			return true;
		} else {
			if (voluntary) {
				save_setjmp(&ls->save, ls, TID_NONE, true, true,
					    true, ADDR_NONE, true, TID_NONE,
					    false, false, false);
			}
			lsprintf(DEV, "ICB count %u bound %u\n",
				 ls->sched.icb_preemption_count, ls->icb_bound);
			FOUND_A_BUG(ls, "Deadlock -- no threads are runnable!\n");
			return false;
		}
	} else {
		lsprintf(DEV, "Deadlock -- no threads are runnable!\n");
		return false;
	}
}
