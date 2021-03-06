/**
 * @file pp.c
 * @brief preemption poince
 * @author Ben Blum
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

#include <stdio.h>  /* file io */
#include <unistd.h> /* unlink */

#define MODULE_NAME "PP"

#include "common.h"
#include "kspec.h"
#include "landslide.h"
#include "pp.h"
#include "stack.h"
#include "student_specifics.h"
#include "x86.h"

void pps_init(struct pp_config *p)
{
	p->dynamic_pps_loaded = false;
	ARRAY_LIST_INIT(&p->kern_withins, 16);
	ARRAY_LIST_INIT(&p->user_withins, 16);
	ARRAY_LIST_INIT(&p->data_races,   16);
	p->output_pipe_filename = NULL;
	p->input_pipe_filename  = NULL;

	/* Load PPs from static config (e.g. if not running under quicksand) */

	static const unsigned int kfuncs[][3] = KERN_WITHIN_FUNCTIONS;
	for (int i = 0; i < ARRAY_SIZE(kfuncs); i++) {
		struct pp_within pp = { .func_start = kfuncs[i][0],
		                        .func_end   = kfuncs[i][1],
		                        .within     = (kfuncs[i][2] != 0) };
		ARRAY_LIST_APPEND(&p->kern_withins, pp);
	}

	static const unsigned int ufuncs[][3] = USER_WITHIN_FUNCTIONS;
	for (int i = 0; i < ARRAY_SIZE(ufuncs); i++) {
		struct pp_within pp = { .func_start = ufuncs[i][0],
		                        .func_end   = ufuncs[i][1],
		                        .within     = (ufuncs[i][2] != 0) };
		ARRAY_LIST_APPEND(&p->user_withins, pp);
	}

	/* [i][0] is instruction pointer of the data race;
	 * [i][1] is the current TID when the race was observed;
	 * [i][2] is the last_call'ing eip value, if any;
	 * [i][3] is the most_recent_syscall when the race was observed. */
	static const unsigned int drs[][4] = DATA_RACE_INFO;
	for (int i = 0; i < ARRAY_SIZE(drs); i++) {
		struct pp_data_race pp = { .addr                = drs[i][0],
		                           .tid                 = drs[i][1],
		                           .last_call           = drs[i][2],
		                           .most_recent_syscall = drs[i][3] };
		ARRAY_LIST_APPEND(&p->data_races, pp);
#ifdef PREEMPT_EVERYWHERE
		assert(0 && "DR PPs incompatible with preempt-everywhere mode.");
#endif
	}
}

bool load_dynamic_pps(struct ls_state *ls, const char *filename)
{
	struct pp_config *p = &ls->pps;
	if (p->dynamic_pps_loaded) {
		return false;
	}

	lsprintf(DEV, "using dynamic PPs from %s\n", filename);
	FILE *pp_file = fopen(filename, "r");
	assert(pp_file != NULL && "failed open pp file");
	char buf[BUF_SIZE];
	while (fgets(buf, BUF_SIZE, pp_file) != NULL) {
		unsigned int x, y, z, w;
		int ret;
		if (buf[strlen(buf) - 1] == '\n') {
			buf[strlen(buf) - 1] = 0;
		}
		if (buf[0] == 'O') { /* capital letter o, not numeral 0 */
			/* expect filename to start immediately after a space */
			assert(buf[1] == ' ');
			assert(buf[2] != ' ' && buf[2] != '\0');
			assert(p->output_pipe_filename == NULL);
			p->output_pipe_filename = MM_XSTRDUP(buf + 2);
			lsprintf(DEV, "output %s\n", p->output_pipe_filename);
		} else if (buf[0] == 'I') {
			/* expect filename to start immediately after a space */
			assert(buf[1] == ' ');
			assert(buf[2] != ' ' && buf[2] != '\0');
			assert(p->input_pipe_filename == NULL);
			p->input_pipe_filename = MM_XSTRDUP(buf + 2);
			lsprintf(DEV, "input %s\n", p->input_pipe_filename);
		} else if ((ret = sscanf(buf, "K %x %x %i", &x, &y, &z)) != 0) {
			/* kernel within function directive */
			assert(ret == 3 && "invalid kernel within PP");
			lsprintf(DEV, "new PP: kernel %x %x %x\n", x, y, z);
			struct pp_within pp = { .func_start = x, .func_end = y,
			                        .within = (z != 0) };
			ARRAY_LIST_APPEND(&p->kern_withins, pp);
		} else if ((ret = sscanf(buf, "U %x %x %i", &x, &y, &z)) != 0) {
			/* user within function directive */
			assert(ret == 3 && "invalid user within PP");
			lsprintf(DEV, "new PP: user %x %x %x\n", x, y, z);
			struct pp_within pp = { .func_start = x, .func_end = y,
			                        .within = (z != 0) };
			ARRAY_LIST_APPEND(&p->user_withins, pp);
		} else if ((ret = sscanf(buf, "DR %x %i %i %i", &x, &y, &z, &w)) != 0) {
			/* data race preemption poince */
			assert(ret == 4 && "invalid data race PP");
			lsprintf(DEV, "new PP: dr %x %x %x %x\n", x, y, z, w);
			struct pp_data_race pp =
				{ .addr = x, .tid = y, .last_call = z,
				  .most_recent_syscall = w };
			ARRAY_LIST_APPEND(&p->data_races, pp);
#ifdef PREEMPT_EVERYWHERE
			assert(0 && "DR PPs incompatible with preempt-everywhere mode.");
#endif
		} else {
			/* unknown */
			lsprintf(DEV, "warning: unrecognized directive in "
				 "dynamic pp config file: '%s'\n", buf);
		}
	}
	fclose(pp_file);

	if (unlink(filename) < 0) {
		lsprintf(DEV, "warning: failed rm temp PP file %s\n", filename);
	}

	p->dynamic_pps_loaded = true;

	messaging_open_pipes(&ls->mess, p->input_pipe_filename,
			     p->output_pipe_filename);
	return true;
}

static bool check_withins(struct ls_state *ls, pp_within_list_t *pps)
{
#ifndef PREEMPT_EVERYWHERE
	/* If there are no within_functions, the default answer is yes.
	 * Otherwise the default answer is no. Later ones take precedence, so
	 * all of them have to be compared. */
	bool any_withins = false;
#endif
	bool answer = true;
	unsigned int i;
	struct pp_within *pp;

	struct stack_trace *st = stack_trace(ls);

	ARRAY_LIST_FOREACH(pps, i, pp) {
		bool in = within_function_st(st, pp->func_start, pp->func_end);
		if (pp->within) {
#ifndef PREEMPT_EVERYWHERE
			/* Switch to whitelist mode. */
			if (!any_withins) {
				any_withins = true;
				answer = false;
			}
#endif
			/* Must be within this function to allow. */
			if (in) {
				answer = true;
			}
		} else {
			/* Must NOT be within this function to allow. */
			if (in) {
				answer = false;
			}
		}
	}

	free_stack_trace(st);
	return answer;
}

bool kern_within_functions(struct ls_state *ls)
{
	return check_withins(ls, &ls->pps.kern_withins);
}

bool user_within_functions(struct ls_state *ls)
{
	return check_withins(ls, &ls->pps.user_withins);
}

#ifdef PREEMPT_EVERYWHERE
#define EBP_OFFSET_HEURISTIC 0x10 /* for judging stack frame accesses */
void maybe_preempt_here(struct ls_state *ls, unsigned int addr)
{
#ifndef TESTING_MUTEXES
	if (ls->sched.cur_agent->action.user_mutex_locking ||
	    ls->sched.cur_agent->action.user_mutex_unlocking ||
	    ls->sched.cur_agent->action.kern_mutex_locking ||
	    ls->sched.cur_agent->action.kern_mutex_trylocking ||
	    ls->sched.cur_agent->action.kern_mutex_unlocking) {
		return;
	}
#endif
	/* Omit accesses on the current stack frame. Also, extend consideration
	 * of the current frame to include up to 4 pushed args. Beyond that is
	 * considered "shared memory". It's ok to have false positives on this
	 * judgement of shared memory as long as they're uncommon; the cost is
	 * just extra PPs that DPOR will find to be independent. But the cost
	 * of false negatives (not preempting on true shms) is missing bugs. */
	if (addr < GET_CPU_ATTR(ls->cpu0, esp) - WORD_SIZE ||
	    addr >= GET_CPU_ATTR(ls->cpu0, ebp) + EBP_OFFSET_HEURISTIC) {
		ls->sched.cur_agent->preempt_for_shm_here = true;
	}
}

bool suspected_data_race(struct ls_state *ls)
{
#ifndef DR_PPS_RESPECT_WITHIN_FUNCTIONS
	assert(0 && "PREEMPT_EVERYWHERE requires DR_PPS_RESPECT_WITHIN_FUNCTIONS");
#endif
	return ls->sched.cur_agent->preempt_for_shm_here;
}

#else

bool suspected_data_race(struct ls_state *ls)
{
	struct pp_data_race *pp;
	unsigned int i;

#ifndef PINTOS_KERNEL
	// FIXME: Make this work for Pebbles kernel-space testing too.
	// Make the condition more precise (include testing_userspace() at least).
	if (!check_user_address_space(ls)) {
		return false;
	}
#endif

	ARRAY_LIST_FOREACH(&ls->pps.data_races, i, pp) {
		if (KERNEL_MEMORY(pp->addr)) {
#ifndef PINTOS_KERNEL
			assert(pp->most_recent_syscall != 0);
#endif
		} else {
			assert(pp->most_recent_syscall == 0);
		}

		if (pp->addr == ls->eip &&
		    (pp->tid == DR_TID_WILDCARD ||
		     pp->tid == ls->sched.cur_agent->tid) &&
		    (pp->last_call == 0 || /* last_call=0 -> anything */
		     pp->last_call == ls->sched.cur_agent->last_call) &&
		    pp->most_recent_syscall == ls->sched.cur_agent->most_recent_syscall) {
			return true;
		}
	}
	return false;
}
#endif
