#include <pthread.h>

/* maximum number of threads allowed in a pool */
#define MAXT_IN_POOL 200

typedef int (*job_routine)(void *);
struct job {
	job_routine jb_routine;	/* the threads process function */
	void *jb_arg;			/* argument to the function */
	struct job *jb_next;
};


struct thread_pool {
	int num_threads;	/*number of active threads */
	int qsize;		/* number in the queue */
	pthread_t *threads;	/* pointer to threads */
	struct job *qhead;		/* queue head pointer */
	struct job *qtail;		/* queue tail pointer */
	pthread_mutex_t qlock;	/* lock on the queue list */
	pthread_cond_t q_not_empty;
				/* non empty and empty condidtion vairiables */
	pthread_cond_t q_empty;
	int shutdown;		/* 1 if the pool is in distruction process */
	int dont_accept;	/* 1 if destroy function has begun */
};


struct thread_pool *thread_pool_new(int num_threads_in_pool);
void dispatch(struct thread_pool * from_me, job_routine job_routine, void *arg);
void thread_pool_delete(struct thread_pool * pool);
