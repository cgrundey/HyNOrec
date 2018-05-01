/**
* NOrec Transactional Memory
*   The five functions of software transactional memory are implemented
*   and used in a Bank Account example.
*   tx_begin()
*   tx_abort()
*   tx_read()
*   tx_write()
*   tx_commit()
* disjoint access to memory is used to compare runtime duration
*
* Author: Colin Grundey
* Date: March 23, 2018
*
* Compile:
*   g++ norec_disjoint_cgrundey.cpp -o norecdisjoint -lpthread -std=c++11
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

#include "rand_r_32.h"

#define CFENCE  __asm__ volatile ("":::"memory")
#define MFENCE  __asm__ volatile ("mfence":::"memory")

#define NUM_ACCTS    1000
#define NUM_TXN      100000
#define TRFR_AMT     50
#define INIT_BALANCE 1000

using namespace std;

typedef struct {
  int addr;
  int value;
} Acct;

vector<Acct> accts;
unsigned int numThreads;
thread_local list<Acct> read_set;
thread_local list<Acct> write_set;
thread_local unsigned int rv = 0;
volatile unsigned int global_clock = 0;

inline unsigned long long get_real_time() {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);
    return time.tv_sec * 1000000000L + time.tv_nsec;
}

/* Aborts from transaction by throwing ann exception of type STM_exception */
void tx_abort() {
  throw "Transaction ABORTED";
}

int tx_validate() {
  int temp;
  while(1) {
    temp = global_clock;
    if ((temp & 1) != 0)
      continue;
    list<Acct>::iterator iterator;
    for (iterator = read_set.begin(); iterator != read_set.end(); ++iterator) {
      if (iterator->value != accts[iterator->addr].value)
        tx_abort();
      if (temp == global_clock)
        return temp;
    }
  }
}

/* Where a transaction begins. Read and write sets are intialized and cleared. */
void tx_begin() {
  read_set.clear();
  write_set.clear();
  do {
    rv = global_clock;
  } while((rv & 1) != 0);
}

/* Adds a version of the account at addr to write_set with the given value. */
bool tx_write(int addr, int val) {
  Acct temp = {addr, val};
  write_set.push_back(temp);
}

/* Adds account with version to read_set and returns the value for later use. */
int tx_read(int addr) {
  list<Acct>::reverse_iterator iterator;
  for (iterator = write_set.rbegin(); iterator != write_set.rend(); ++iterator) {
    if (iterator->addr == addr)
      return iterator->value;
  }
  int val = accts[addr].value;
  while(rv != global_clock) {
    rv = tx_validate();
    val = accts[addr].value;
  }
  Acct temp = {addr, val};
  read_set.push_back(temp);
  return val;
}

/* Attempts to commit the transaction. Checks read and write set versions
 * and compares it to memory versions (i.e. accts[].ver). If valid, all
 * accounts in read and write sets are written back to memory. */
void tx_commit() {
  if (write_set.empty())
    return;
  while(!__sync_bool_compare_and_swap(&global_clock, rv, rv + 1))
    rv = tx_validate();
  /* Validation is a success */
  list<Acct>::iterator iterator;
  for (iterator = write_set.begin(); iterator != write_set.end(); ++iterator) {
    accts[iterator->addr].value = iterator->value;
  }
  global_clock = rv + 2;
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
  bool aborted = false;
  int start;
  int thrange = NUM_ACCTS / numThreads;
  start = thrange * (id-1);
  int workload = NUM_TXN / numThreads;
  for (int i = 0; i < workload; i++) {
    printf("Txn: %d\n", i+1);
    do {
      aborted = false;
      try {
        tx_begin();
        int r1 = 0;
        int r2 = 0;
        for (int j = 0; j < 10; j++) {
          while (r1 == r2) {
            r1 = start + (rand_r_32(&tid) % (thrange-1));
            r2 = start + (rand_r_32(&tid) % (thrange-1));
          }
          // Perform the transfer
          int a1 = tx_read(r1);
          if (a1 < TRFR_AMT)
            break;
          int a2 = tx_read(r2);
          tx_write(r1, a1 - TRFR_AMT);
          tx_write(r2, a2 + TRFR_AMT);
        }
        tx_commit();
      } catch(const char* msg) {
        printf("ABORTED: %s\n", msg);
        aborted = true;
      }
    } while (aborted);
// _________________END__________________
  }
  return 0;
}

int main(int argc, char* argv[]) {
  // Input arguments error checking
  if (argc != 2) {
    printf("Usage: <# of threads -> 1, 2, or 4\n");
    exit(0);
  } else {
    numThreads = atoi(argv[1]);
    if (numThreads != 1 && numThreads != 2 && numThreads != 4) {
      printf("Usage: <# of threads -> 1, 2, or 4\n");
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

  // Thread initializations
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);

  pthread_t client_th[300];
  long ids = 1;
  for (int i = 0; i < numThreads; i++) {
    pthread_create(&client_th[ids-1], &thread_attr, th_run, (void*)ids);
    ids++;
  }

/* EXECUTION BEGIN */
  unsigned long long start = get_real_time();
  th_run(0);
  // Joining Threads
  for (int i=0; i<numThreads; i++) {
    pthread_join(client_th[i], NULL);
  }
/* EXECUTION END */
  long totalMoneyAfter = 0;
  for (int i = 0; i < NUM_ACCTS; i++)
    totalMoneyAfter += accts[i].value;

  printf("\nTotal time = %lld ns\n", get_real_time() - start);
  printf("Total Money Before: $%ld\n", totalMoneyBefore);
  printf("Total Money After:  $%ld\n", totalMoneyAfter);

  return 0;
}
