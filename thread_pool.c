#include <stdlib.h>
#include <stdio.h>
#include "thread_pool.h"

static void *do_the_job(void *arg)
{
	int err;
	struct job *job;
	struct thread_pool *pool = arg;

	while (1) {
		if ((err = pthread_mutex_lock(&pool->qlock)))
			perror("pthread_mutex_lock error in do_the_job");

		if (pool->shutdown)
			goto out;

		/*
		 * when dont_accept flags is set, all of the threads won't wait
		 * the struct job (request from the client) any more.
		 */
		while (!pool->dont_accept && !pool->qhead) {
			if ((err = pthread_cond_wait(&pool->q_not_empty,
						     &pool->qlock)))
				perror("pthread_cond_wait error in do_the_job");

			if (pool->shutdown)
				goto out;
	
		}
	
		if (pool->qhead) {
			pool->qsize--;
			job = pool->qhead;/* get a job and update the qhead. */
			pool->qhead = pool->qhead->jb_next;
		}
		
		if ((err = pthread_mutex_unlock(&pool->qlock)))
			perror("pthread_mutex_unlock error in do_the_job");

		/*
		 * when dont_accept flags is set, and pool->qhead is NULL(all
		 * of the job is finished). we signal to destroy process.
		 */	
		if (pool->dont_accept && !pool->qhead) {
			if ((err = pthread_cond_signal(&pool->q_empty)))
				perror("pthread_cond_signal error in do_the_job");
		}
		
		if (job) {
			/* start the job, and process the request from client. */
			job->jb_routine(job->jb_arg);		
			/* when using -lpthread, thread safe version is used */
			free(job);
		}
	}

out:
	pool->num_threads--;
	if ((err = pthread_mutex_unlock(&pool->qlock)))
		perror("pthread_mutex_unlock error in do_the_job");

	return NULL;
}

void dispatch(struct thread_pool *from_me, job_routine job_routine, void *arg)
{
	int s;
	struct job *job = NULL;

	if ( !from_me | !job_routine)
		goto out;

	if (!(job = calloc(1, sizeof(*job)))) {
		perror("calloc error in dispatch");
		goto out;
	}

	job->jb_routine = job_routine;
	job->jb_arg = arg;

	if (from_me->dont_accept)
		goto out;

	if ((s = pthread_mutex_lock(&from_me->qlock)))
		perror("pthread_mutex_lock error in dispatch");

	/* 
	 * qhead empty means there not job in the queue. 
	 */
	from_me->qsize++;
	if (from_me->qhead && from_me->qtail) {
		from_me->qtail->jb_next = job;
		from_me->qtail = from_me->qtail->jb_next;
	} else {
		/* all of the job has been done. add new job to qhead. */
		from_me->qhead = job;
		from_me->qtail = job;
	}

	if ((s = pthread_mutex_unlock(&from_me->qlock)))
		perror("pthread_mutex_unlock error in dispatch");
	
	if ((s = pthread_cond_signal(&from_me->q_not_empty)))
		perror("pthread_cond_signal error in dispatch");

	return;
out:
	if (job)
		free(job);
}

void thread_pool_delete(struct thread_pool *pool)
{
	int i, err, num_threads_saved;

	if (!pool)
		return;

       	num_threads_saved = pool->num_threads;
	if ((err = pthread_mutex_lock(&pool->qlock)))	/* LOCK */
		perror("pthread_mutex_lock error in thread_pool_delete");

	/*
	 * first, set dont_accept flags to 1, this will cause all the thread
	 * don't to wait new struct job object. and when all of the struct job
	 * structure has been finished. it will signal us goto next step.
	 */
	pool->dont_accept = 1;
	
	/* if have job(struct job) to do. wait it become NULL */
	while (pool->qsize > 0) {
		if ((err = pthread_cond_wait(&pool->q_empty,
					     &pool->qlock)))
			perror("pthread_cond_wait error in thread_pool_delete");
	}

	if ((err = pthread_mutex_unlock(&pool->qlock)))	/* UNLOCK */
		perror("pthread_mutex_unlock error in thread_pool_delete");

	/* 
	 * atfer all struct job structure has been finished. we set the shutdown
	 * flags to 1, lets all of the threads to go out the while, and finally
	 * return NULL to exit.
	 */
	pool->shutdown = 1;
	if ((err = pthread_cond_broadcast(&pool->q_not_empty)))
		perror("pthread_mutex_unlock error in thread_pool_delete");
	
	/*
	 * join all of the threads we created before. and also, we don't use
	 * return value. just leave it NULL.
	 */
	for (i = 0; i < num_threads_saved; i++) {
		if ((err = pthread_join(pool->threads[i], NULL)))
			perror("pthread_join error in destroy_threadpoll");
	}

	/* free memory resource which allocated by malloc(). */
	if (pool->threads)
		free(pool->threads);
	if (pool)
		free(pool);	
}

struct thread_pool *thread_pool_new(int num_threads_in_pool)
{
	int err;
	int i;
	struct thread_pool *pool = calloc(1, sizeof(*pool));

	if (num_threads_in_pool <= 0 || num_threads_in_pool > MAXT_IN_POOL) {
		fprintf(stderr, "invalid arguments: %d (%d-%d)\n",
				num_threads_in_pool, 1, MAXT_IN_POOL);
		goto out;
	}

	if (!pool) {
		perror("allocate memory for thread pool error");
		goto out;
	}

	/* use to store the thread pointer to array. */
	if (!(pool->threads = calloc(num_threads_in_pool,
				     sizeof(*(pool->threads))))) {
		perror("allocate memory for threads array error");
		goto out;
	}

	if ((err = pthread_mutex_init(&pool->qlock, NULL)))
		goto out;

	if ((err = pthread_cond_init(&pool->q_not_empty, NULL)))
		goto out;

	if ((err = pthread_cond_init(&pool->q_empty, NULL)))
		goto out;

	/*
	 * create a number of thread, which specified by 'num_threads_in_pool'.
	 */
	pool->num_threads = num_threads_in_pool;
	for (i = 0; i < pool->num_threads; i++) {
		if ((err = pthread_create(&pool->threads[i], NULL, do_the_job,
					  pool)))
			goto out;
	}


	return pool;
out:
	thread_pool_delete(pool);
	return NULL;
}

