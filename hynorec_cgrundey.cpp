/**
* Hybrid NOrec Transactional Memory
*    Special purpose Hybrid NOrec
*       This implementation tries to use Intel's HTM
*       up to 5 times and then uses NOrec as a fallback
*       with concurrency between the two.
*
* Author: Colin Grundey
* Date: April 2018
*
* Compile:
*   g++ hynorec_cgrundey.cpp -o hynorec -lpthread -std=c++11
*   add -g option for gdb
*/

#include <pthread.h>
#include <cstdlib>
#include <vector>
#include <signal.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <iostream>
#include <time.h>
#include <list> // Linked list for read/write sets
#include <errno.h>

#include "rtm.h"
#include "rand_r_32.h"

#define CFENCE  __asm__ volatile ("":::"memory")
#define MFENCE  __asm__ volatile ("mfence":::"memory")

#define CACHELINE_BYTES 64

#define NUM_ACCTS    1000
#define NUM_TXN      100000
#define TRFR_AMT     50
#define INIT_BALANCE 1000

using namespace std;

struct pad_word_t
{
  volatile uintptr_t val;
  char pad[CACHELINE_BYTES-sizeof(uintptr_t)];
};

typedef struct {
  int addr;
  int value;
} Acct;

vector<Acct> accts;
int numThreads;
thread_local list<Acct> read_set;
thread_local list<Acct> write_set;
thread_local unsigned int rv = 0;
thread_local pad_word_t snap_counter[72];
volatile unsigned int seqlock = 0;
pad_word_t counter[72];

inline unsigned long long get_real_time() {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);
    return time.tv_sec * 1000000000L + time.tv_nsec;
}

void sw_abort() {
  read_set.clear();
  write_set.clear();
  throw "Transaction ABORTED";
}

void sw_validate() {
  do {
    do {
      rv = seqlock;
    } while (rv & 1);
    list<Acct>::iterator iterator;
    for (iterator = read_set.begin(); iterator != read_set.end(); ++iterator) {
      if (iterator->value != accts[iterator->addr].value)
        sw_abort();
    }
  } while (rv != seqlock);
}

void sw_begin() {
  do {
    rv = seqlock;
  } while (rv & 1);
  std::copy(std::begin(counter), std::end(counter), std::begin(snap_counter));
}

void sw_commit() {
  if (write_set.empty())
    return;
  while(!__sync_bool_compare_and_swap(&seqlock, rv, rv + 1))
    sw_validate();
  for (int i = 0; i < 72; i++) {
    if (snap_counter[i].val != counter[i].val) {
      sw_validate();
      break;
    }
  }
  /* WRITEBACK */
  list<Acct>::iterator iterator;
  for (iterator = write_set.begin(); iterator != write_set.end(); ++iterator) {
    accts[iterator->addr].value = iterator->value;
  }
  seqlock++;
  read_set.clear();
  write_set.clear();
}

int sw_read(int addr) {
  list<Acct>::reverse_iterator iterator;
  for (iterator = write_set.rbegin(); iterator != write_set.rend(); ++iterator) {
    if (iterator->addr == addr)
      return iterator->value;
  }
  int val = accts[addr].value;
  while(rv != seqlock) {
    sw_validate();
    val = accts[addr].value;
  }
  Acct temp = {addr, val};
  read_set.push_back(temp);
  return val;
}

void sw_write(int addr, int val) {
  Acct temp = {addr, val};
  write_set.push_back(temp);
}

/* Support a few lightweight barriers */
void barrier(int which) {
    static volatile int barriers[16] = {0};
    CFENCE;
    __sync_fetch_and_add(&barriers[which], 1);
    while (barriers[which] < numThreads) { }
    CFENCE;
}

// Thread function
void* th_run(void * args)
{
  long id = (long)args;
  unsigned int tid = (unsigned int)(id);
  barrier(0);

// ________________BEGIN_________________
  unsigned int htmCount = 0;
  unsigned int swCount = 0;

  bool aborted = false;
  int workload = NUM_TXN / numThreads;
  for (int i = 0; i < workload; i++) {
    do {
      int attempts = 5;
      again: aborted = false;
//________________HW_TRANSACTION___________________________
      unsigned int status = _xbegin();
      if (status == _XBEGIN_STARTED) {
        if (seqlock & 1) // HW_POST_BEGIN
          _xabort(1);
        for (int j = 0; j < 10; j++) {
          int r1 = 0;
          int r2 = 0;
          while (r1 == r2) {
            r1 = rand_r_32(&tid) % NUM_ACCTS;
            r2 = rand_r_32(&tid) % NUM_ACCTS;
          }
          int a1 = accts[r1].value;
          if (a1 < TRFR_AMT)
            break;
          int a2 = accts[r2].value;
          accts[r1].value = a1 - TRFR_AMT;
          accts[r2].value = a2 - TRFR_AMT;
        }
        counter[tid].val += 1;// HW_PRE_COMMIT
        _xend();
        htmCount++;
      } else if (attempts > 0) {
        attempts--;
        goto again;
      }
//________________SW_TRANSACTION___________________________
      else {
        try {
          sw_begin();
          for (int j = 0; j < 10; j++) {
            int r1 = 0;
            int r2 = 0;
            while (r1 == r2) {
              r1 = rand_r_32(&tid) % NUM_ACCTS;
              r2 = rand_r_32(&tid) % NUM_ACCTS;
            }
            // Perform the transfer
            int a1 = sw_read(r1);
            if (a1 < TRFR_AMT)
              break;
            int a2 = sw_read(r2);
            sw_write(r1, a1 - TRFR_AMT);
            sw_write(r2, a2 + TRFR_AMT);
          }
          sw_commit();
          swCount++;
        } catch(const char* msg) {
          aborted = true;
        }
      }
    } while (aborted);
// _________________END__________________
  }
  printf("Thread ID: %ld\tHardware Count: %d\tSoftware Count: %d\tTotal: %d\n", id, htmCount, swCount, htmCount + swCount);
  return 0;
}

int main(int argc, char* argv[])
{
//	signal(SIGINT, signal_callback_handler);

  // Input arguments error checking
  if (argc != 2) {
    printf("Usage: <# of threads 1-64>\n");
    exit(0);
  } else {
    numThreads = atoi(argv[1]);
    if (numThreads <= 0 || numThreads > 64) {
      printf("Usage: <# of threads 1-64>\n");
      exit(0);
    }
  }
  printf("Number of threads: %d\n", numThreads);

  // Initializing 1,000,000 accounts with $1000 each
  for (int i = 0; i < NUM_ACCTS; i++) {
    Acct temp = {i, INIT_BALANCE};
    accts.push_back(temp);
  }

  long totalMoneyBefore = 0;
  for (int i = 0; i < NUM_ACCTS; i++)
    totalMoneyBefore += accts[i].value;

	pthread_attr_t thread_attr;
	pthread_attr_init(&thread_attr);

	pthread_t client_th[300];
	long ids = 1;
	for (int i = 1; i<numThreads; i++) {
		pthread_create(&client_th[ids-1], &thread_attr, th_run, (void*)ids);
		ids++;
	}

	unsigned long long start = get_real_time();

	th_run(0);

	for (int i=0; i<ids-1; i++) {
		pthread_join(client_th[i], NULL);
	}
  /* EXECUTION END */
  long totalMoneyAfter = 0;
  for (int i = 0; i < NUM_ACCTS; i++)
      totalMoneyAfter += accts[i].value;

  printf("Total time = %lld ns\n", get_real_time() - start);
  printf("Total Money Before: $%ld\n", totalMoneyBefore);
  printf("Total Money After:  $%ld\n", totalMoneyAfter);

	return 0;
}
