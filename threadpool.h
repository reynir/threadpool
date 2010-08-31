
#ifndef THREADPOOL_H
#  define THREADPOOL_H

typedef struct ThreadQueue_ *ThreadQueue;

int threadpool_init( void );
int threadpool_newJob( int (*function)(void *), void *data );

ThreadQueue vpool_create( void );
void vpool_enqueue( ThreadQueue queue, int (*function)(void *), void *data );
void vpool_wait( ThreadQueue queue );



#endif
