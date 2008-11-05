/*
 * Copyright (C) 2008 Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * $Id$
 */

#include "mutex.h"

#include <library.h>
#include <debug.h>
#include <utils/backtrace.h>

#include <pthread.h>
#include <sys/time.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

/**
 * Do not report mutexes with an overall waiting time smaller than this (in us)
 */
#define PROFILE_TRESHHOLD 1000

typedef struct private_mutex_t private_mutex_t;
typedef struct private_r_mutex_t private_r_mutex_t;
typedef struct private_condvar_t private_condvar_t;

/**
 * private data of mutex
 */
struct private_mutex_t {

	/**
	 * public functions
	 */
	mutex_t public;
	
	/**
	 * wrapped pthread mutex
	 */
	pthread_mutex_t mutex;
	
#ifdef LOCK_PROFILER
	/**
	 * how long threads have waited for the lock in this mutex so far
	 */
	struct timeval waited;
	
	/**
	 * backtrace where mutex has been created
	 */
	backtrace_t *backtrace;
#endif /* LOCK_PROFILER */
	
	/**
	 * is this a recursiv emutex, implementing private_r_mutex_t?
	 */
	bool recursive;
};

/**
 * private data of mutex, extended by recursive locking information
 */
struct private_r_mutex_t {

	/**
	 * Extends private_mutex_t
	 */
	private_mutex_t generic;
	
	/**
	 * thread which currently owns mutex
	 */
	pthread_t thread;
	
	/**
	 * times we have locked the lock, stored per thread
	 */
	pthread_key_t times;
};

/**
 * private data of condvar
 */
struct private_condvar_t {

	/**
	 * public functions
	 */
	condvar_t public;
	
	/**
	 * wrapped pthread condvar
	 */
	pthread_cond_t condvar;
};

#ifdef LOCK_PROFILER
/**
 * Print and cleanup mutex profiler
 */
static void profiler_cleanup(private_mutex_t *this)
{
	if (this->waited.tv_sec > 0 ||
		this->waited.tv_usec > PROFILE_TRESHHOLD)
	{
		fprintf(stderr, "waited %d.%06ds in mutex, created at:",
				this->waited.tv_sec, this->waited.tv_usec);
		this->backtrace->log(this->backtrace, stderr);
	}
	this->backtrace->destroy(this->backtrace);
}

/**
 * Initialize mutex profiler
 */
static void profiler_init(private_mutex_t *this)
{
	this->backtrace = backtrace_create(3);
	timerclear(&this->waited);
}

/**
 * Implementation of mutex_t.lock.
 */
static void lock(private_mutex_t *this)
{
	struct timeval start, end, diff;

	gettimeofday(&start, NULL);
	if (pthread_mutex_lock(&this->mutex))
	{
		DBG1("!!!! MUTEX %sLOCK ERROR, your code is buggy !!!", "");
	}
	gettimeofday(&end, NULL);
	
	timersub(&end, &start, &diff);
	timeradd(&this->waited, &diff, &this->waited);
}
#else /* !LOCK_PROFILER */

/** dummy implementations */
static void profiler_cleanup(private_mutex_t *this) {}
static void profiler_init(private_mutex_t *this) {}

/**
 * Implementation of mutex_t.lock.
 */
static void lock(private_mutex_t *this)
{
	if (pthread_mutex_lock(&this->mutex))
	{
		DBG1("!!!! MUTEX %sLOCK ERROR, your code is buggy !!!", "");
	}
}
#endif /* LOCK_PROFILER */

/**
 * Implementation of mutex_t.unlock.
 */
static void unlock(private_mutex_t *this)
{
	if (pthread_mutex_unlock(&this->mutex))
	{
		DBG1("!!!! MUTEX %sLOCK ERROR, your code is buggy !!!", "UN");
	}
}

/**
 * Implementation of mutex_t.lock.
 */
static void lock_r(private_r_mutex_t *this)
{
	pthread_t self = pthread_self();

	if (this->thread == self)
	{
		uintptr_t times;
		
		/* times++ */
		times = (uintptr_t)pthread_getspecific(this->times);
		pthread_setspecific(this->times, (void*)times + 1);
	}
	else
	{
		lock(&this->generic);
		this->thread = self;
		/* times = 1 */
		pthread_setspecific(this->times, (void*)1);
	}
}

/**
 * Implementation of mutex_t.unlock.
 */
static void unlock_r(private_r_mutex_t *this)
{
	uintptr_t times;

	/* times-- */
	times = (uintptr_t)pthread_getspecific(this->times);
	pthread_setspecific(this->times, (void*)--times);
	
	if (times == 0)
	{
		this->thread = 0;
		unlock(&this->generic);
	}
}

/**
 * Implementation of mutex_t.destroy
 */
static void mutex_destroy(private_mutex_t *this)
{
	profiler_cleanup(this);
	pthread_mutex_destroy(&this->mutex);
	free(this);
}

/**
 * Implementation of mutex_t.destroy for recursive mutex'
 */
static void mutex_destroy_r(private_r_mutex_t *this)
{
	profiler_cleanup(&this->generic);
	pthread_mutex_destroy(&this->generic.mutex);
	pthread_key_delete(this->times);
	free(this);
}

/*
 * see header file
 */
mutex_t *mutex_create(mutex_type_t type)
{
	switch (type)
	{
		case MUTEX_RECURSIVE:
		{
			private_r_mutex_t *this = malloc_thing(private_r_mutex_t);
			
			this->generic.public.lock = (void(*)(mutex_t*))lock_r;
			this->generic.public.unlock = (void(*)(mutex_t*))unlock_r;
			this->generic.public.destroy = (void(*)(mutex_t*))mutex_destroy_r;	
			
			pthread_mutex_init(&this->generic.mutex, NULL);
			pthread_key_create(&this->times, NULL);
			this->generic.recursive = TRUE;
			profiler_init(&this->generic);
			this->thread = 0;
			
			return &this->generic.public;
		}
		case MUTEX_DEFAULT:
		default:
		{
			private_mutex_t *this = malloc_thing(private_mutex_t);
		
			this->public.lock = (void(*)(mutex_t*))lock;
			this->public.unlock = (void(*)(mutex_t*))unlock;
			this->public.destroy = (void(*)(mutex_t*))mutex_destroy;
			
			pthread_mutex_init(&this->mutex, NULL);
			this->recursive = FALSE;
			profiler_init(this);
			
			return &this->public;
		}
	}
}

/**
 * Implementation of condvar_t.wait.
 */
static void wait(private_condvar_t *this, private_mutex_t *mutex)
{
	if (mutex->recursive)
	{
		private_r_mutex_t* recursive = (private_r_mutex_t*)mutex;
		
		/* mutex owner gets cleared during condvar wait */
		recursive->thread = 0;
		pthread_cond_wait(&this->condvar, &mutex->mutex);
		recursive->thread = pthread_self();
	}
	else
	{
		pthread_cond_wait(&this->condvar, &mutex->mutex);
	}
}

/**
 * Implementation of condvar_t.timed_wait.
 */
static bool timed_wait(private_condvar_t *this, private_mutex_t *mutex,
					   u_int timeout)
{
	struct timespec ts;
	struct timeval tv;
	u_int s, ms;
	bool timed_out;
	
	gettimeofday(&tv, NULL);
	
	s = timeout / 1000;
	ms = timeout % 1000;
	
	ts.tv_sec = tv.tv_sec + s;
	ts.tv_nsec = tv.tv_usec * 1000 + ms * 1000000;
	if (ts.tv_nsec > 1000000000 /* 1s */)
	{
		ts.tv_nsec -= 1000000000;
		ts.tv_sec++;
	}
	if (mutex->recursive)
	{
		private_r_mutex_t* recursive = (private_r_mutex_t*)mutex;
		
		recursive->thread = 0;
		timed_out = pthread_cond_timedwait(&this->condvar, &mutex->mutex,
										   &ts) == ETIMEDOUT;
		recursive->thread = pthread_self();
	}
	else
	{
		timed_out = pthread_cond_timedwait(&this->condvar, &mutex->mutex,
										   &ts) == ETIMEDOUT;
	}
	return timed_out;
}

/**
 * Implementation of condvar_t.signal.
 */
static void signal(private_condvar_t *this)
{
	pthread_cond_signal(&this->condvar);
}

/**
 * Implementation of condvar_t.broadcast.
 */
static void broadcast(private_condvar_t *this)
{
	pthread_cond_broadcast(&this->condvar);
}

/**
 * Implementation of condvar_t.destroy
 */
static void condvar_destroy(private_condvar_t *this)
{
	pthread_cond_destroy(&this->condvar);
	free(this);
}

/*
 * see header file
 */
condvar_t *condvar_create(condvar_type_t type)
{
	switch (type)
	{
		case CONDVAR_DEFAULT:
		default:
		{
			private_condvar_t *this = malloc_thing(private_condvar_t);
		
			this->public.wait = (void(*)(condvar_t*, mutex_t *mutex))wait;
			this->public.timed_wait = (bool(*)(condvar_t*, mutex_t *mutex, u_int timeout))timed_wait;
			this->public.signal = (void(*)(condvar_t*))signal;
			this->public.broadcast = (void(*)(condvar_t*))broadcast;
			this->public.destroy = (void(*)(condvar_t*))condvar_destroy;
		
			pthread_cond_init(&this->condvar, NULL);
		
			return &this->public;
		}
	}
}

