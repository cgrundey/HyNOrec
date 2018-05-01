#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>

#include <errno.h>
#include "rtm.h"

#define CFENCE  __asm__ volatile ("":::"memory")
#define MFENCE  __asm__ volatile ("mfence":::"memory")


volatile int counter = 0;
volatile int lock = 0;

int total_threads;


inline unsigned long long get_real_time() {
        struct timespec time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);

    return time.tv_sec * 1000000000L + time.tv_nsec;
}

/**
 *  Support a few lightweight barriers
 */
void
barrier(int which)
{
    static volatile int barriers[16] = {0};
    CFENCE;
    __sync_fetch_and_add(&barriers[which], 1);
    while (barriers[which] != total_threads) { }
    CFENCE;
}

void
signal_callback_handler(int signum)
{
   // Terminate program
   exit(signum);
}

volatile bool ExperimentInProgress = true;
static void catch_SIGALRM(int sig_num)
{
    ExperimentInProgress = false;
}

void* th_run(void * args)
{

	long id = (long)args;
	int localCounter = 0;

	barrier(0);
		
	int inHTM = 0;
	int inGL = 0;
	for (int i=0; i<1000000; i++) {
		int attempts = 5;
		again: while (lock == 1) {}
		unsigned int status = _xbegin();
		if(status == _XBEGIN_STARTED){
			if (lock == 1) _xabort(1);
			//Code for HTM path
			counter++;
			_xend();
			inHTM++;
		} else if (attempts > 0) {
			attempts--;
			goto again;
		} else {
			while(!__sync_bool_compare_and_swap(&lock, 0, 1)){}
				//Code for SW path with instrumented read/write
				counter++;
			lock = 0;
			inGL++;
		}
	}
 	printf("Thread %ld local counter = %d and global counter = %d. In HTM = %d, in GL = %d\n", id, localCounter, counter, inHTM, inGL); 
	return 0;
}

int main(int argc, char* argv[])
{
//	signal(SIGINT, signal_callback_handler);

	if (argc < 2) {
		printf("Usage test threads#\n");
		exit(0);
	}

    total_threads = atoi(argv[1]);

	pthread_attr_t thread_attr;
	pthread_attr_init(&thread_attr);

	pthread_t client_th[300];
	long ids = 1;
	for (int i = 1; i<total_threads; i++) {
		pthread_create(&client_th[ids-1], &thread_attr, th_run, (void*)ids);
		ids++;
	}
	
	unsigned long long start = get_real_time();

	th_run(0);

	for (int i=0; i<ids-1; i++) {
		pthread_join(client_th[i], NULL);
	}
	
	printf("Total time = %lld ns\n", get_real_time() - start);
	printf("Counter = %d\n", counter);

	return 0;
}

//Build with 
//g++ test_threads.cpp -o test -lpthread