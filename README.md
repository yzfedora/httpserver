# httpserver
The thread-pool implementation under the Linux or Unix-like system.

# how to use the thread pool api?
	- just inclide thread_pool.[ch] files to your source tree.
	- there is a global allowed maximum threads to running in the header.
		#define MAXT_IN_POOL	200
	- functions
		struct thread_pool *thread_pool_new(int num_threads_in_pool);
		void dispatch(struct thread_pool * from_me, job_routine job_routine, void *arg);
		void thread_pool_delete(struct thread_pool * pool);

	- and there is a simple http server code in the source tree. also, it
	  explains how to use this thread pool APIs.
