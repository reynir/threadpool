/*
 * See Licensing and Copyright notice in naev.h
 */

#ifdef LOG_H
#include "log.h" /* For debugging in naev */
#endif

#include "SDL.h"
#include "SDL_thread.h"

#include <stdlib.h>

#define THREADPOOL_TIMEOUT (30 * 1000)


#if SDL_VERSION_ATLEAST(1,3,0)
const int MAXTHREADS = SDL_GetCPUCount();
#else
const int MAXTHREADS = 8;
#endif


typedef struct Node_ *Node;
typedef struct Node_ {
   void *data;
   Node next;
} Node_;

typedef struct ThreadQueue_ {
   Node first;
   Node last;
   SDL_sem *semaphore; 
   SDL_mutex *mutex;
} ThreadQueue_;
typedef ThreadQueue_ *ThreadQueue;

typedef struct ThreadQueue_data_ {
   int (*function)(void *);
   void *data;
} ThreadQueue_data_;
typedef ThreadQueue_data_ *ThreadQueue_data;

typedef struct ThreadData_ {
   int (*function)(void *);
   void *data;
   SDL_sem *semaphore;
   ThreadQueue idle;
   ThreadQueue stopped;
} ThreadData_;

typedef struct vpoolThreadData_ {
	SDL_cond *cond;
   SDL_mutex *mutex;
   int *count;
   ThreadQueue_data node;
} vpoolThreadData_;
typedef vpoolThreadData_ *vpoolThreadData;


static ThreadQueue queue = NULL;

ThreadQueue tq_create()
{
   ThreadQueue q = malloc( sizeof(ThreadQueue_) );

   q->first = NULL;
   q->last = NULL;
   q->mutex = SDL_CreateMutex();
   q->semaphore = SDL_CreateSemaphore( 0 );

   return q;
}


void tq_enqueue( ThreadQueue q, void *data )
{
   Node n;

   /* Lock */
   SDL_mutexP(q->mutex);

   n = malloc(sizeof(Node_));
   n->data = data;
   n->next = NULL;
   if (q->first == NULL)
      q->first = n;
   else
      q->last->next = n;
   q->last = n;

   /* Signal and unlock.
    * This wil break if someone tries to enqueue 2^32+1 elements or something.
    * */
   SDL_SemPost(q->semaphore);
   SDL_mutexV(q->mutex);
}

void* tq_dequeue( ThreadQueue q )
{
   void *d;
   Node temp;
   
   /* Lock */
   SDL_mutexP(q->mutex);

   if (q->first == NULL) {
      #ifdef LOG_H
      WARN("Tried to dequeue while the queue was empty. This is really SEVERE!");
      #endif
      /* Unlock to prevent a deadlock */
      SDL_mutexV(q->mutex);
      return NULL;
   }

   d = q->first->data;
   temp = q->first;
   q->first = q->first->next;
   if (q->first == NULL)
      q->last = NULL;
   free(temp);
   
   /* Unlock */
   SDL_mutexV(q->mutex);
   return d;
}

void tq_destroy( ThreadQueue q )
{
   SDL_DestroySemaphore(q->semaphore);
   SDL_DestroyMutex(q->mutex);

   /* Shouldn't it free the dequeued nodes? */
   while(q->first != NULL) {
      tq_dequeue(q);
   }
   free(q);
}

/* Eh, threadsafe? nah */
int tq_isEmpty( ThreadQueue q )
{
   if (q->first == NULL)
      return 1;
   else
      return 0;
}


/* Enqueues a new job for the threadpool. Do NOT enqueue a job that has to wait
 * for another job to be done as this could lead to a deadlock. */
int threadpool_newJob(int (*function)(void *), void *data)
{
   ThreadQueue_data node;
   if (queue == NULL) {
      #ifdef LOG_H
      WARN("threadpool.c: Threadpool has not been initialized yet!");
      #endif
      return -2;
   }
   
   node = malloc( sizeof(ThreadQueue_data_) );
   node->data = data;
   node->function = function;

   tq_enqueue( queue, node );
   
   return 0;
}

int threadpool_worker( void *data )
{
   ThreadData_ *work;
   
   work = (ThreadData_ *) data;

   while (1) {
      /* Break if timeout or signal to stop */
      if (SDL_SemWaitTimeout( work->semaphore, THREADPOOL_TIMEOUT ) != 0) {
         break;
      }

      (*work->function)( work->data );
      
      tq_enqueue( work->idle, work );
   }
   /* TODO: The folowing might break stuff! The thread is still in the idle
    * queue so  the threadpool_handler might have given the thread work while
    * the  threadpool_worker was about to break the while loop. A fix would be
    * to  use a different data structure that allows the threadpool_worker to
    * remove itself from the idle queue. This means I should write a different
    * tq_enqueue function that returns the Node that the threadpool_worker is
    * in. There should also be a tq_trytoremovemefromthisqueue-function that
    * takes a Node and tries to remove the Node from the queue. */
   tq_enqueue( work->stopped, work );

   return 0;
}

int threadpool_handler( void *data )
{
   int i;
   ThreadData_ *threadargs, *threadarg;
   /* Queues for idle workers and stopped workers */
   ThreadQueue idle, stopped;
   ThreadQueue_data node;

   threadargs = malloc( sizeof(ThreadData_)*MAXTHREADS );

   idle = tq_create();
   stopped = tq_create();

   /* Initialize threadargs */
   for (i=0; i<MAXTHREADS; i++) {
      threadargs[i].function = NULL;
      threadargs[i].data = NULL;
      threadargs[i].semaphore = SDL_CreateSemaphore( 0 );
      threadargs[i].idle = idle;
      threadargs[i].stopped = stopped;

      tq_enqueue(stopped, &threadargs[i]);
   }

   while (1) {
      SDL_SemWait( queue->semaphore ); /* Wait for new job(s) in queue */
      node = tq_dequeue( queue );

      if( SDL_SemTryWait(idle->semaphore) == 0) {
         /* Idle thread available */
         threadarg = tq_dequeue( idle );
         threadarg->function = node->function;
         threadarg->data = node->data;
         SDL_SemPost( threadarg->semaphore );

      } else if( SDL_SemTryWait(stopped->semaphore) == 0) {
         /* Make new thread */
         threadarg = tq_dequeue(stopped);
         threadarg->function = node ->function;
         threadarg->data = node->data;
         SDL_SemPost( threadarg->semaphore );

         SDL_CreateThread( threadpool_worker, threadarg );

      } else {
         /* Wait for idle thread */
         SDL_SemWait(idle->semaphore);
         threadarg = tq_dequeue( idle );
         threadarg->function = node->function;
         threadarg->data = node->data;
         SDL_SemPost( threadarg->semaphore );
      }
      free(node);
   }
   /* TODO: cleanup and maybe a way to stop the threadpool */
}

int threadpool_init()
{
   if (queue != NULL) {
      #ifdef LOG_H
      WARN("Threadpool has already been initialized!");
      #endif
      return -1;
   }

   queue = tq_create();

   SDL_CreateThread( threadpool_handler, NULL );

   return 0;
}

/* Creates a new vpool queue */
ThreadQueue vpool_create()
{
   return tq_create();
}

/* Enqueue a job in the vpool queue. Do NOT enqueue a job that has to wait for
 * another job to be done as this could lead to a deadlock. */
void vpool_enqueue(ThreadQueue queue, int (*function)(void *), void *data)
{
   ThreadQueue_data node;
   
   node = malloc( sizeof(ThreadQueue_data_) );
   node->data = data;
   node->function = function;
   
   tq_enqueue( queue, node );
}

int vpool_worker(void *data)
{
   vpoolThreadData work;
   
   work = (vpoolThreadData) data;

   /* Do work */
   work->node->function( work->node->data );

   /* Count down the counter and signal vpool_wait if all threads are done */
   SDL_mutexP( work->mutex );
   *(work->count) = *(work->count) - 1;
   if (*(work->count) == 0)
      SDL_CondSignal( work->cond );
   SDL_mutexV( work->mutex );

   /* Cleanup */
   free(work->node);
   free(work);

   return 0;
}

/* Run every job in the vpool queue and block untill every job in the queue is
 * done. It destroys the queue when it's done. */
void vpool_wait(ThreadQueue queue)
{
   int i, count;
   SDL_cond *cond;
   SDL_mutex *mutex;
   vpoolThreadData arg;
   ThreadQueue_data node;

   cond = SDL_CreateCond();
   mutex = SDL_CreateMutex();
   count = SDL_SemValue( queue->semaphore );

   SDL_mutexP( mutex );
   for (i=0; i<count; i++) {
      SDL_SemWait( queue->semaphore );
      node = tq_dequeue( queue );
      arg = malloc( sizeof(vpoolThreadData_) );

      arg->node = node;
      arg->cond = cond;
      arg->mutex = mutex;
      arg->count = &count;

      threadpool_newJob( vpool_worker, arg );
   }

   /* Wait for the threads to finish */
   SDL_CondWait( cond, mutex );
   SDL_mutexV( mutex );

   /* Clean up */
   SDL_DestroyMutex( mutex );
   SDL_DestroyCond( cond );
   tq_destroy( queue );
}

/* Notes
 *
 * The algorithm/strategy for killing idle workers should be moved into the
 * threadhandler and it should also be improved (the current strategy is
 * probably not very good).
 */
