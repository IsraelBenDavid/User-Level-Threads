/********** INCLUDES **********/

#include <stdio.h>
#include <iostream>
#include <stdlib.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <vector>
#include <queue>
#include "uthreads.h"



/********** MESSAGES **********/

#define LIB_ERROR "thread library error: "
#define SYS_ERROR "system error: "

#define SIGEMPT_FAILURE "Sigemptyset failed.\n"
#define SIGACTION_FAILURE "Sigaction failed.\n"
#define SETITIMER_FAILURE "Setitimer failed.\n"
#define ALLOC_FAIURE "Memory allocation failed.\n"
#define SIGBLOCK_FAILURE "Sigprocmask failed.\n"

#define THREAD_NOT_FOUND_ERROR "Thread not found.\n"
#define QSEC_ERROR "Quantum usec must be positive.\n"
#define ENTRYPOINT_ERROR "Received invalid entry point.\n"
#define MAXTHREAD_ERROR "Reached max thread amount.\n"
#define MAIN_BLOCK_ERROR "Cannot block main thread.\n"
#define MAIN_SLEEP_ERROR "Cannot put main thread to sleep.\n"


/********** CONSTANTS **********/

#define TRUE 0
#define FALSE (-1)
#define NOT_SLEEPING (-1)
#define NOT_EXIST (-1)

/********** STRUCTURES *********/

typedef struct Thread {
    int tid;
    int running_quantums;
    char *stack;
    thread_entry_point entry;
    int sleep_remain;
    bool is_resumed;

    sigjmp_buf env;
} Thread;

typedef std::vector<Thread *> thread_list;

/********** PRIVATE GLOBAL VARIABLES **********/

sigset_t set;
int next_tid_available = 0;
int total_quantum;
thread_list ready_threads;
thread_list blocked_threads;
Thread *running_thread;
struct itimerval timer; // quantum timer
std::priority_queue<int, std::vector<int>, std::greater<int>> min_id_heap;

/********** PRIVATE FUNCTIONS **********/


#ifdef __x86_64__
/* code for 64 bit Intel arch */
typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address (address_t addr)
{
  address_t ret;
  asm volatile("xor    %%fs:0x30,%0\n"
               "rol    $0x11,%0\n"
      : "=g" (ret)
      : "0" (addr));
  return ret;
}
#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5


/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
                 "rol    $0x9,%0\n"
    : "=g" (ret)
    : "0" (addr));
    return ret;
}
#endif

void block_timer_signal ()
{
  if (sigprocmask (SIG_BLOCK, &set, nullptr))
    {
      fprintf (stderr, SYS_ERROR SIGBLOCK_FAILURE);
      exit (1);
    }
}

void unblock_timer_signal ()
{
  if (sigprocmask (SIG_UNBLOCK, &set, nullptr))
    {
      fprintf (stderr, SYS_ERROR SIGBLOCK_FAILURE);
      exit (1);
    }
}

// free single thread
void free_thread (Thread *thread)
{
  free (thread->stack);
  thread->stack = NULL;
}

// free a list of threads
void free_thread_list (thread_list *t_list)
{
  for (auto elem: *t_list)
    {
      free_thread (elem);
      free (elem);
    }
  t_list->clear ();
}

// free the whole system
void free_system ()
{
  free_thread_list (&ready_threads);
  free_thread_list (&blocked_threads);
  free_thread (running_thread);
}

void setup_thread (Thread *thread)
{
  // initializes env[tid] to use the right stack, and to run from the function 'entry_point', when we'll use
  // siglongjmp to jump into the thread.
  address_t sp = (address_t) thread->stack + STACK_SIZE - sizeof (address_t);
  auto pc = (address_t) thread->entry;
  sigsetjmp(thread->env, 1);
  (thread->env->__jmpbuf)[JB_SP] = translate_address (sp);
  (thread->env->__jmpbuf)[JB_PC] = translate_address (pc);
  if (sigemptyset (&(thread->env)->__saved_mask) == FALSE)
    {
      free_system ();
      free_thread (thread);
      free (thread);
      fprintf (stderr, SYS_ERROR SIGEMPT_FAILURE);
      exit (1);
    }
}

void setup_main_thread ()
{
  if (sigemptyset (&(running_thread->env)->__saved_mask) == FALSE)
    {
      free_system ();
      fprintf (stderr, SYS_ERROR SIGEMPT_FAILURE);
      exit (1);
    }
}

/* Returns the position of thread with tid in the list */
int tid_to_index (thread_list *list, int tid)
{
  for (int i = 0; i < list->size (); ++i)
    {
      if (list->at (i)->tid == tid)
        {
          return i;
        }
    }
  return NOT_EXIST; // not found
}

/* Replaces currently running thread with the next ready thread */
void run_next ()
{
  if (ready_threads.empty ())
    { return; }
  running_thread = ready_threads.at (0);
  running_thread->running_quantums++;
  ready_threads.erase (ready_threads.begin ());
}

/* Let the threads know a quantum of sleeping has passed */
void wake_up_threads ()
{
  for (auto elem: blocked_threads)
    {
      if (elem->sleep_remain > 0)
        { elem->sleep_remain--; }
      if (elem->sleep_remain == 0)
        {
          elem->sleep_remain = NOT_SLEEPING;
          if (elem->is_resumed)
            { uthread_resume (elem->tid); }
        }
    }
}

void queue_step ()
{
  if (running_thread != NULL
      && tid_to_index (&blocked_threads, running_thread->tid) == NOT_EXIST)
    {
      ready_threads.push_back (running_thread);
    }
  run_next ();
  total_quantum++;
  wake_up_threads ();
  unblock_timer_signal();
  siglongjmp (running_thread->env, 1);
}

void save_time_vars (int sig)
{
  int ret_val = sigsetjmp(running_thread->env, 1);

  bool did_just_save_bookmark = ret_val == 0;
  if (did_just_save_bookmark)
    {
      queue_step ();
    }
}

void init_quantum_timer (int quantum_usecs)
{
  struct sigaction sa = {0};
  // Install timer_handler as the signal handler for SIGVTALRM.
  sa.sa_handler = &save_time_vars;
  if (sigaction (SIGVTALRM, &sa, NULL) < 0)
    {
      fprintf (stderr, SYS_ERROR SIGACTION_FAILURE);
      exit (1);
    }

  /* Set system timer */
  // first interval
  timer.it_value.tv_sec = quantum_usecs / 1000000;
  timer.it_value.tv_usec = quantum_usecs % 1000000;
  // following time intervals
  timer.it_interval.tv_sec = quantum_usecs / 1000000;
  timer.it_interval.tv_usec = quantum_usecs % 1000000;


  // Start a virtual timer. It counts down whenever this process is executing.
  if (setitimer (ITIMER_VIRTUAL, &timer, NULL))
    {
      fprintf (stderr, SYS_ERROR SETITIMER_FAILURE);
      exit (1);
    }
}

/* Creates new thread */
Thread *create_thread (thread_entry_point entry_point)
{
  auto new_thread = (Thread *) calloc (1, sizeof (Thread));
  if (new_thread == NULL)
    {
      free_system ();
      fprintf (stderr, SYS_ERROR ALLOC_FAIURE);
      exit (1);
    }
  new_thread->entry = entry_point;
  new_thread->tid = min_id_heap.top ();
  new_thread->sleep_remain = NOT_SLEEPING;
  new_thread->stack = (char *) calloc (STACK_SIZE, sizeof (char));
  if (new_thread->stack == NULL)
    {
      free (new_thread);
      free_system ();
      fprintf (stderr, SYS_ERROR ALLOC_FAIURE);
      exit (1);
    }
  min_id_heap.pop ();
  setup_thread (new_thread);
  return new_thread;
}


/* External interface */



/**
 * @brief initializes the thread library.
 *
 * Once this function returns, the main thread (tid == 0) will be set as RUNNING. There is no need to
 * provide an entry_point or to create a stack for the main thread - it will be using the "regular" stack and PC.
 * You may assume that this function is called before any other thread library function, and that it is called
 * exactly once.
 * The input to the function is the length of a quantum in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_init (int quantum_usecs)
{
  if (quantum_usecs <= 0)
    {
      fprintf (stderr, LIB_ERROR QSEC_ERROR);
      return FALSE;
    }
  total_quantum = 1;

  // set the tid heap
  for (int i = 1; i < MAX_THREAD_NUM; i++)
    { min_id_heap.push (i); }

  sigemptyset (&set);
  sigaddset (&set, SIGVTALRM);


  /* initialize main thread */
  running_thread = (Thread *) calloc (1, sizeof (Thread));


  if (running_thread == NULL)
    {
      fprintf (stderr, SYS_ERROR ALLOC_FAIURE);
      exit (1);
    }
  running_thread->running_quantums++;
  running_thread->sleep_remain = NOT_SLEEPING;
  next_tid_available++;
  setup_main_thread ();
  init_quantum_timer (quantum_usecs);
  return TRUE;
}

/**
 * @brief Creates a new thread, whose entry point is the function entry_point with the signature
 * void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
 * limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 * It is an error to call this function with a null entry_point.
 *
 * @return On success, return the ID of the created thread. On failure, return -1.
*/
int uthread_spawn (thread_entry_point entry_point)
{
  block_timer_signal ();
  if (entry_point == NULL)
    {
      fprintf (stderr, LIB_ERROR ENTRYPOINT_ERROR);
      unblock_timer_signal ();
      return FALSE;
    }

  // thread amount limit
  if (ready_threads.size () + blocked_threads.size () + 1 == MAX_THREAD_NUM)
    {
      fprintf (stderr, LIB_ERROR MAXTHREAD_ERROR);
      unblock_timer_signal ();
      return FALSE;
    }
  auto new_thread = create_thread (entry_point);
  ready_threads.push_back (new_thread);
  unblock_timer_signal ();
  return new_thread->tid;
}

/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
 *
 * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
 * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
 * process using exit(0) (after releasing the assigned library memory).
 *
 * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
 * itself or the main thread is terminated, the function does not return.
*/
int uthread_terminate (int tid)
{
  block_timer_signal ();
  if (tid == 0)
    { // Terminating the main thread
      free_system ();
      exit (0);
    }

  int tid_index_ready_list = tid_to_index (&ready_threads, tid);
  int tid_index_blocked_list = tid_to_index (&blocked_threads, tid);
  min_id_heap.push (tid); //reuse id before freeing

  if (tid_index_ready_list != NOT_EXIST)
    { // in ready list
      free_thread (ready_threads.at (tid_index_ready_list));
      free (ready_threads.at (tid_index_ready_list));
      ready_threads.erase (ready_threads.begin () + tid_index_ready_list);
      unblock_timer_signal ();
      return TRUE;
    }
  if (tid_index_blocked_list != NOT_EXIST)
    { // in block list
      free_thread (blocked_threads.at (tid_index_blocked_list));
      free (blocked_threads.at (tid_index_blocked_list));
      blocked_threads.erase (
          blocked_threads.begin () + tid_index_blocked_list);
      unblock_timer_signal ();
      return TRUE;
    }

  if (running_thread->tid == tid)
    { // it is the running thread
      free_thread (running_thread);
      free (running_thread); // terminate
      running_thread = NULL;
      queue_step ();

    }
  // not exist
  fprintf (stderr, LIB_ERROR THREAD_NOT_FOUND_ERROR);
  unblock_timer_signal ();
  return FALSE;

}

/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_block (int tid)
{
  block_timer_signal ();
  if (tid == 0)
    {
      fprintf (stderr, LIB_ERROR MAIN_BLOCK_ERROR);
      unblock_timer_signal ();
      return FALSE;
    }

  int ready_thread = tid_to_index (&ready_threads, tid);
  int blocked_thread = tid_to_index (&blocked_threads, tid);

  if (blocked_thread != NOT_EXIST)
    {
      unblock_timer_signal ();
      return TRUE;
    }
  if (ready_thread != NOT_EXIST)
    { // if the tread is ready
      ready_threads.at (ready_thread)->is_resumed = false;
      blocked_threads.push_back (ready_threads.at (ready_thread));
      ready_threads.erase (ready_threads.begin () + ready_thread);
      unblock_timer_signal ();
      return TRUE;
    }
  if (running_thread->tid == tid)
    { // if the tread is running
      running_thread->is_resumed = true;
      if (running_thread->sleep_remain == NOT_SLEEPING){
        running_thread->is_resumed = false;
      }
      blocked_threads.push_back (running_thread);

      // reset timer
      if (setitimer (ITIMER_VIRTUAL, &timer, NULL))
        {
          free_system ();
          fprintf (stderr, SYS_ERROR SETITIMER_FAILURE);
          exit (1);
        }
      save_time_vars (5);
      unblock_timer_signal ();
      return TRUE;
    }

  if (running_thread->tid != tid)
    {
      fprintf (stderr, LIB_ERROR THREAD_NOT_FOUND_ERROR);
      unblock_timer_signal ();
      return FALSE;
    }
  unblock_timer_signal ();
  return TRUE;
}

/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_resume (int tid)
{
  block_timer_signal ();
  if (running_thread->tid == tid)
    {
      unblock_timer_signal();
      return TRUE;
    }
  if (tid_to_index (&ready_threads, tid) != NOT_EXIST)
    {
      unblock_timer_signal();
      return TRUE;
    }

  int ready_thread = tid_to_index (&ready_threads, tid);
  int blocked_thread = tid_to_index (&blocked_threads, tid);

  // if thread is not blocked
  if (blocked_thread == NOT_EXIST)
    {
      if (ready_thread == NOT_EXIST & running_thread->tid != tid)
        {
          fprintf (stderr, LIB_ERROR THREAD_NOT_FOUND_ERROR);
          unblock_timer_signal ();
          return FALSE;
        }
      else
        {
          unblock_timer_signal ();
          return TRUE;
        }
    }

  //only if tread is blocked
  blocked_threads.at (blocked_thread)->is_resumed = true;
  //only if tread is not sleeping
  if (blocked_threads.at (blocked_thread)->sleep_remain == NOT_SLEEPING)
    {
      ready_threads.push_back (blocked_threads.at (blocked_thread));
      blocked_threads.erase (blocked_threads.begin () + blocked_thread);
    }
  unblock_timer_signal ();
  return TRUE;
}

/**
 * @brief Blocks the RUNNING thread for num_quantums quantums.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the READY queue.
 * If the thread which was just RUNNING should also be added to the READY queue, or if multiple threads wake up
 * at the same time, the order in which they're added to the end of the READY queue doesn't matter.
 * The number of quantums refers to the number of times a new quantum starts, regardless of the reason. Specifically,
 * the quantum of the thread which has made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid == 0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_sleep (int num_quantums)
{
  block_timer_signal ();
  if (running_thread->tid == 0)
    {
      fprintf (stderr, LIB_ERROR MAIN_SLEEP_ERROR);
      unblock_timer_signal ();
      return FALSE;
    }
  running_thread->sleep_remain = num_quantums;
  uthread_block (running_thread->tid);
  unblock_timer_signal ();
  return TRUE;
}

/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
*/
int uthread_get_tid ()
{
  return running_thread->tid;
}

/**
 * @brief Returns the total number of quantums since the library was initialized, including the current quantum.
 *
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number should be increased by 1.
 *
 * @return The total number of quantums.
*/
int uthread_get_total_quantums ()
{
  return total_quantum;
}

/**
 * @brief Returns the number of quantums the thread with ID tid was in RUNNING state.
 *
 * On the first time a thread runs, the function should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state when this function is called, include
 * also the current quantum). If no thread with ID tid exists it is considered an error.
 *
 * @return On success, return the number of quantums of the thread with ID tid. On failure, return -1.
*/
int uthread_get_quantums (int tid)
{
  block_timer_signal ();
  if (running_thread->tid == tid)
    {
      unblock_timer_signal ();
      return running_thread->running_quantums;
    }
  int ready_thread_ind = tid_to_index (&ready_threads, tid);
  int blocked_thread_ind = tid_to_index (&blocked_threads, tid);

  if (ready_thread_ind != NOT_EXIST)
    {
      unblock_timer_signal ();
      return ready_threads.at (ready_thread_ind)->running_quantums;
    }
  else if (blocked_thread_ind != NOT_EXIST)
    {
      unblock_timer_signal ();
      return blocked_threads.at (blocked_thread_ind)->running_quantums;
    }

  fprintf (stderr, LIB_ERROR THREAD_NOT_FOUND_ERROR);

  unblock_timer_signal ();
  return FALSE;
}

