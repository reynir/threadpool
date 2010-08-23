/*
 * Hello, world!
 * 
 * The queue stuff is mostly stolen from queue.c
 */

#include "SDL.h"
#include "SDL_thread.h"
#include <stdlib.h>

#define MAXTHREADS 32

typedef struct Node_ *Node;
typedef struct Node_ {
   ThreadQueue_data *data;
   Node next;
} Node_;

typedef struct ThreadQueue_ {
   Node first;
   Node last;
   SDL_sem semaphore; // Old
   SDL_mutex mutex; // Old
   SDL_cond cond;
} *ThreadQueue; // Can I do this???

typedef struct ThreadData_ {
   int (*function)(void *);
   void *data;
   SDL_sem semaphore;
   ThreadQueue queue;
} ThreadData_;

typedef struct ThreadQueue_data_ {
   int (*fn)(void *) function;
   void *data;
} ThreadQueue_data;

static ThreadQueue *queue = NULL;

ThreadQueue tq_create()
{
   ThreadQueue q = malloc( sizeof(ThreadQueue) );

   q->first = NULL;
   q->last = NULL;
   q->mutex = SDL_CreateMutex();
   q->cond = SDL_CreateCond();
   q->semaphore = SDL_CreateSemaphore( 0 );

   return q;
}

void tq_destroy( ThreadQueue q )
{
   /* TODO: This function should somehow signal the thread handler that
    * the queue is being destroyed.
    * Uh, thread handler should be the only one to call this! */
   SDL_DestroyMutex( q->mutex );
   SDL_DestroyCond( q->cond );

   while(q->first != NULL) {
      tq_dequeue(q);
   }
   free(q);
}

void tq_enqueue( ThreadQueue q, void *data )
{
   Node n;

   SDL_mutexP(q->mutex); // Lock

   n = malloc(sizeof(Node_));
   n->data = data;
   n->next = NULL;
   if (q->first == NULL)
      q->first = n;
   else
      q->last->next = n;
   q->last = n;

   SDL_CondSignal(q->cond); // Signal thread handler there's new work.
   SDL_SemPost() //TODO
   SDL_mutexV(q->mutex); // Unlock

   return; // wow, redundant!
}

void* tq_dequeue( ThreadQueue q )
{
   void *d;
   Node temp;
   
   SDL_mutexP(q->mutex); // Lock

   if (q->first == NULL) {
      SDL_mutexV(q->mutex); // Unlock. We don't want a dead lock!
      return NULL;
   }

   d = q->first->data;
   temp = q->first;
   q->first = q->first->next;
   if (q->first == NULL)
      q->last = NULL;
   free(temp);
   
   SDL_mutexV(q->mutex); // Unlock
   return d;
}

/* Eh, threadsafe? nah */
int q_isEmpty( ThreadQueue q )
{
   if (q->first == NULL)
      return 1;
   else
      return 0;
}


int threadpool_init()
{
   /* Something fishy is happening :) */
   if (queue != NULL)
      return -1;

   queue = tq_create();

    SDL_CreateThread( threadpool_handler, NULL );
   
   return 0;
}

/**/
int threadpool_newJob(int (*function)(void *), void *data)
{
   ThreadQueue_data node;
   if (queue == NULL)
      return -2; // No threadpool has been made!
   
   node = malloc( sizeof(ThreadQueue_data) );
   node->data = data;
   node->function = function;

   tq_enqueue( queue, node );
   
   return 0;
}

int threadpool_worker( void *data )
{
   ThreadData_ *work;
   
   work = (ThreadData_ *) data;

   while (true) {
      SDL_SemWait( work->semaphore );
      // Do work
      (*work->function)( work->data );
      
      tq_enqueue( work->queue, work );
   }
}

int threadpool_handler( void *data )
{
   SDL_thread *thread_stack; // Stack of workers, in case magic happens.
   ThreadData_ *threadargs, *threadarg;
   int thread_nstack;
   ThreadQueue idle; // Queue for idle workers.
   ThreadQueue_data node;

   threadargs = malloc( sizeof(ThreadData_)*MAXTHREADS );
   thread_stack = malloc( sizeof(SDL_thread)*MAXTHREADS ); // Will allocate MAXTHREADS for now.
   thread_nstack = -1;

   idle = tq_create();

   while (true) {
      SDL_SemWait( queue->semaphore ); // Wait for new job(s) in queue
      
      node = (ThreadQueue_data *) tq_dequeue( queue );

      if( thread_nstack < MAXTHREADS-1 ) {
         int res = 123; // Eh
         res = SDL_SemTryWait( idle->semaphore ); // Is there already an idle thread?
         if(res==0) {
            // We got an idle worker in idle
            threadarg = tq_dequeue( idle );
            threadarg->function = node->function;
            threadarg->data = node->data;
            SDL_SemPost( threadarg->semaphore );
         } else {
            // Make new thread and give it work
            thread_nstack++;
            threadargs[thread_nstack]->function = node ->function;
            threadargs[thread_nstack]->data = node->data;
            threadargs[thread_nstack]->semaphore = SDL_CreateSemaphore( 1 );
            threadargs[thread_nstack]->queue = &idle;

            thread_stack[thread_nstack] = SDL_CreateThread( threadpool_worker, threadargs[thread_nstack] );
         }
      } else {
         SDL_SemWait( idle->semaphore ); // Wait for idle thread
         threadarg = tq_dequeue( idle );
         threadarg->function = node->function;
         threadarg->data = node->data;
         SDL_SemPost( threadarg->semaphore );
      }
   }
}