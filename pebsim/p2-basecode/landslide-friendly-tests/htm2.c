/** @file 410user/progs/htm2.c
 *  @author bblum
 *  @brief like htm1 but without the bug
 *  @public yes
 *  @for p2
 *  @covers lol
 *  @status done
 */

/* Includes */
#include <assert.h>
#include <syscall.h>
#include <stdlib.h>
#include <thread.h>
#include <mutex.h>
#include <cond.h>
#include "410_tests.h"
#include <report.h>
#include <test.h>
#include <htm.h>

DEF_TEST_NAME("htm2:");

#define ERR REPORT_FAILOUT_ON_ERR

mutex_t lock;
volatile static int count = 0;
/* http://www.contrib.andrew.cmu.edu/~mdehesaa/ */
volatile int stop_the_world = 0;

#define NITERS 3
#define NTHREADS 2

void txn()
{
	int status;
	if ((status = _xbegin()) == _XBEGIN_STARTED) {
		if (stop_the_world != 0) {
			_xabort(0);
		}
		count++;
		_xend();
	} else {
		mutex_lock(&lock);
		stop_the_world = 1;
		count++;
		stop_the_world = 0;
		mutex_unlock(&lock);
	}
}

void *child(void *dummy)
{
	for (int i = 0; i < NITERS; i++) {
		txn();
	}
	return NULL;
}

int main(void)
{
	report_start(START_CMPLT);

	ERR(thr_init(4096));
	ERR(mutex_init(&lock));
	misbehave(BGND_BRWN >> FGND_CYAN); // for landslide

	int tid[NTHREADS];
	for (int i = 0; i < NTHREADS - 1; i++) {
		tid[i] = thr_create(child, NULL);
		ERR(tid[i]);
	}

	child(NULL);
	for (int i = 0; i < NTHREADS - 1; i++) {
		ERR(thr_join(tid[i], NULL));
	}
	assert(count == NITERS * NTHREADS);


	return 0;
}
