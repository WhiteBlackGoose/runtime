/*
 * threads-pthread.c: System-specific thread support
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2001 Ximian, Inc.
 */

#include <config.h>
#include <glib.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#include <mono/metadata/object.h>
#include <mono/metadata/threads.h>

/*
 * Implementation of timed thread joining from the P1003.1d/D14 (July 1999)
 * draft spec, figure B-6.
 */
typedef struct {
	pthread_t id;
	MonoObject *object;
	pthread_mutex_t join_mutex;
	pthread_cond_t exit_cond;
	void *(*start_routine)(void *arg);
	void *arg;
	void *status;
	gboolean exiting;
} ThreadInfo;

static pthread_key_t timed_thread_key;
static pthread_once_t timed_thread_once = PTHREAD_ONCE_INIT;

static GHashTable *threads=NULL;
static MonoObject *main_thread;

static void timed_thread_init()
{
	pthread_key_create(&timed_thread_key, NULL);
}

static void timed_thread_exit(void *status)
{
	ThreadInfo *thread;
	void *specific;
	
	if((specific = pthread_getspecific(timed_thread_key)) == NULL) {
		/* Handle cases which won't happen with correct usage.
		 */
		pthread_exit(NULL);
	}
	
	thread=(ThreadInfo *)specific;
	
	pthread_mutex_lock(&thread->join_mutex);
	
	/* Tell a joiner that we're exiting.
	 */
	thread->status = status;
	thread->exiting = TRUE;

	pthread_cond_signal(&thread->exit_cond);
	pthread_mutex_unlock(&thread->join_mutex);
	
	/* Call pthread_exit() to call destructors and really exit the
	 * thread.
	 */
	pthread_exit(NULL);
}

/* Routine to establish thread specific data value and run the actual
 * thread start routine which was supplied to timed_thread_create()
 */
static void *timed_thread_start_routine(void *args)
{
	ThreadInfo *thread = (ThreadInfo *)args;
	
	pthread_once(&timed_thread_once, timed_thread_init);
	pthread_setspecific(timed_thread_key, (void *)thread);
	timed_thread_exit((thread->start_routine)(thread->arg));

	/* pthread_create routine has to return something to keep gcc
	 * quiet
	 */
	return(NULL);
}

/* Allocate a thread which can be used with timed_thread_join().
 */
static int timed_thread_create(ThreadInfo **threadp,
			       const pthread_attr_t *attr,
			       void *(*start_routine)(void *), void *arg)
{
	ThreadInfo *thread;
	int result;
	
	thread=(ThreadInfo *)g_new0(ThreadInfo, 1);
	pthread_mutex_init(&thread->join_mutex, NULL);
	pthread_cond_init(&thread->exit_cond, NULL);
	thread->start_routine = start_routine;
	thread->arg = arg;
	thread->status = NULL;
	thread->exiting = FALSE;
	
	if((result = pthread_create(&thread->id, attr,
				    timed_thread_start_routine,
				    (void *)thread)) != 0) {
		g_free(thread);
		return(result);
	}
	
	pthread_detach(thread->id);
	*threadp = thread;
	return(0);
}

static int timed_thread_join(ThreadInfo *thread, struct timespec *timeout,
			     void **status)
{
	int result;
	
	pthread_mutex_lock(&thread->join_mutex);
	result=0;
	
	/* Wait until the thread announces that it's exiting, or until
	 * timeout.
	 */
	while(result == 0 && !thread->exiting) {
		if(timeout == NULL) {
			result = pthread_cond_wait(&thread->exit_cond,
						   &thread->join_mutex);
		} else {
			result = pthread_cond_timedwait(&thread->exit_cond,
							&thread->join_mutex,
							timeout);
		}
	}
	
	pthread_mutex_unlock(&thread->join_mutex);
	if(result == 0 && thread->exiting) {
		if(status!=NULL) {
			*status = thread->status;
		}
	}
	return(result);
}

pthread_t ves_icall_System_Threading_Thread_Start_internal(MonoObject *this,
							   MonoObject *start)
{
	MonoClassField *field;
	void *(*start_func)(void *);
	ThreadInfo *thread;
	int ret;
	
#ifdef THREAD_DEBUG
	g_message("Trying to start a new thread: this (%p) start (%p)",
		  this, start);
#endif

	field=mono_class_get_field_from_name(mono_defaults.delegate_class, "method_ptr");
	start_func= *(gpointer *)(((char *)start) + field->offset);
	
	if(start_func==NULL) {
		g_warning("Can't locate start method!");
		/* Not sure if 0 can't be a valid pthread_t.  Calling
		 * pthread_self() on the main thread seems to always
		 * return 1024 for me
		 */
		return(0);
	} else {
		ret=timed_thread_create(&thread, NULL, start_func, NULL);
		if(ret!=0) {
			g_warning("pthread_create error: %s", strerror(ret));
			return(0);
		}
	
#ifdef THREAD_DEBUG
		g_message("Started thread ID %ld", thread->id);
#endif

		/* Store tid for cleanup later */
		if(threads==NULL) {
			threads=g_hash_table_new(g_int_hash, g_int_equal);
		}

		thread->object=this;
		
		/* FIXME: need some locking around here */
		g_hash_table_insert(threads, &thread->id, thread);
		
		return(thread->id);
	}
}

gint32 ves_icall_System_Threading_Thread_Sleep_internal(gint32 ms)
{
	struct timespec req, rem;
	div_t divvy;
	int ret;
	
#ifdef THREAD_DEBUG
	g_message("Sleeping for %d ms", ms);
#endif

	divvy=div(ms, 1000);
	
	req.tv_sec=divvy.quot;
	req.tv_nsec=divvy.rem*1000;
	
	ret=nanosleep(&req, &rem);
	if(ret<0) {
		/* Sleep interrupted with rem time remaining */
		gint32 rems=rem.tv_sec*1000 + rem.tv_nsec/1000;
		
#ifdef THREAD_DEBUG
		g_message("Returning %dms early", rems);
#endif
		return(rems);
	}
	
#ifdef THREAD_DEBUG
	g_message("Slept");
#endif
	
	return(0);
}

void ves_icall_System_Threading_Thread_Schedule_internal(void)
{
	/* Give up the timeslice. pthread_yield() is the standard
	 * function but glibc seems to insist it's a GNU
	 * extension. However, all it does at the moment is
	 * sched_yield() anyway, and sched_yield() is a POSIX standard
	 * function.
	 */
	
	sched_yield();
}

MonoObject *ves_icall_System_Threading_Thread_CurrentThread_internal(void)
{
	pthread_t tid;
	ThreadInfo *thread_info;
	
	/* Find the current thread id */
	tid=pthread_self();
	
	/* Look it up in the threads hash */
	if(threads!=NULL) {
		thread_info=g_hash_table_lookup(threads, &tid);
	} else {
		/* No threads running yet! */
		thread_info=NULL;
	}
	
	/* Return the object associated with it */
	if(thread_info==NULL) {
		/* If we can't find our own thread ID, assume it's the
		 * main thread
		 */
		return(main_thread);
	} else {
		return(thread_info->object);
	}
}

static void delete_thread(ThreadInfo *thread)
{
	g_assert(threads!=NULL);
	g_assert(thread!=NULL);
	
	g_hash_table_remove(threads, &thread->id);
	g_free(thread);
}

gboolean ves_icall_System_Threading_Thread_Join_internal(MonoObject *this,
							 int ms, pthread_t tid)
{
	ThreadInfo *thread;
	pthread_t myid;
	int ret;
	
#ifdef THREAD_DEBUG
	g_message("Joining with thread %p id %ld, waiting for %dms", this,
		  tid, ms);
#endif

	myid=pthread_self();
	if(myid==tid) {
		/* .net doesnt spot this and proceeds to
		 * deadlock. This will have to be commented out if we
		 * want to be bug-compatible :-(
		 */
		g_warning("Can't join my own thread!");
		return(FALSE);
	}
	
	if(threads!=NULL) {
		thread=g_hash_table_lookup(threads, &tid);
	} else {
		/* No threads running yet! */
		thread=NULL;
	}
	
	if(thread==NULL) {
		g_warning("Can't find thread id %ld", tid);
		return(FALSE);
	}
	
	if(ms==0) {
		/* block until thread exits */
		ret=timed_thread_join(thread, NULL, NULL);
		
		if(ret==0) {
			delete_thread(thread);
			return(TRUE);
		} else {
			g_warning("Join error: %s", strerror(ret));
			return(FALSE);
		}
	} else {
		/* timeout in ms milliseconds */
		struct timespec timeout;
		time_t now;
		div_t divvy;
		
		divvy=div(ms, 1000);
		now=time(NULL);
		
		timeout.tv_sec=now+divvy.quot;
		timeout.tv_nsec=divvy.rem*1000;
		
		ret=timed_thread_join(thread, &timeout, NULL);
		if(ret==0) {
			delete_thread(thread);
			return(TRUE);
		} else {
			if(ret!=ETIMEDOUT) {
				g_warning("Timed join error: %s",
					  strerror(ret));
			}
			return(FALSE);
		}
	}
}

static void join_all_threads(gpointer key, gpointer value, gpointer user)
{
	ThreadInfo *thread_info=(ThreadInfo *)value;
	
#ifdef THREAD_DEBUG
	g_message("[%ld]", thread_info->id);
#endif
	timed_thread_join(thread_info, NULL, NULL);
}

static gboolean free_all_threadinfo(gpointer key, gpointer value, gpointer user)
{
	g_free(value);
	
	return(TRUE);
}

void mono_thread_init(void)
{
	MonoClass *thread_class;
	
	/* Build a System.Threading.Thread object instance to return
	 * for the main line's Thread.CurrentThread property.
	 */
	thread_class=mono_class_from_name(mono_defaults.corlib, "System.Threading", "Thread");
	
	/* I wonder what happens if someone tries to destroy this
	 * object? In theory, I guess the whole program should act as
	 * though exit() were called :-)
	 */
	main_thread=mono_new_object(thread_class);
}


void mono_thread_cleanup(void)
{
	/* join each thread that's still running */
#ifdef THREAD_DEBUG
	g_message("Joining each running thread...");
#endif
	
	if(threads==NULL) {
#ifdef THREAD_DEBUG
		g_message("No threads");
#endif
		return;
	}
	
	g_hash_table_foreach(threads, join_all_threads, NULL);

	g_hash_table_foreach_remove(threads, free_all_threadinfo, NULL);
	g_hash_table_destroy(threads);
	threads=NULL;
}
