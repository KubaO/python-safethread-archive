
/* Posix threads interface */

#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) || defined(HAVE_PTHREAD_DESTRUCTOR)
#define destructor xxdestructor
#endif
#include <pthread.h>
#if defined(__APPLE__) || defined(HAVE_PTHREAD_DESTRUCTOR)
#undef destructor
#endif
#include <signal.h>

#include <sys/time.h>
#include <time.h>
#include <math.h>

/* The POSIX spec requires that use of pthread_attr_setstacksize
   be conditional on _POSIX_THREAD_ATTR_STACKSIZE being defined. */
#ifdef _POSIX_THREAD_ATTR_STACKSIZE
#ifndef THREAD_STACK_SIZE
#define	THREAD_STACK_SIZE	0	/* use default stack size */
#endif
/* for safety, ensure a viable minimum stacksize */
#define	THREAD_STACK_MIN	0x8000	/* 32kB */
#else  /* !_POSIX_THREAD_ATTR_STACKSIZE */
#ifdef THREAD_STACK_SIZE
#error "THREAD_STACK_SIZE defined but _POSIX_THREAD_ATTR_STACKSIZE undefined"
#endif
#endif

/* The POSIX spec says that implementations supporting the sem_*
   family of functions must indicate this by defining
   _POSIX_SEMAPHORES. */   
#ifdef _POSIX_SEMAPHORES
/* On FreeBSD 4.x, _POSIX_SEMAPHORES is defined empty, so 
   we need to add 0 to make it work there as well. */
#if (_POSIX_SEMAPHORES+0) == -1
#define HAVE_BROKEN_POSIX_SEMAPHORES
#else
#include <semaphore.h>
#include <errno.h>
#endif
#endif

/* Before FreeBSD 5.4, system scope threads was very limited resource
   in default setting.  So the process scope is preferred to get
   enough number of threads to work. */
#ifdef __FreeBSD__
#include <osreldate.h>
#if __FreeBSD_version >= 500000 && __FreeBSD_version < 504101
#undef PTHREAD_SYSTEM_SCHED_SUPPORTED
#endif
#endif

#if !defined(pthread_attr_default)
#  define pthread_attr_default ((pthread_attr_t *)NULL)
#endif
#if !defined(pthread_mutexattr_default)
#  define pthread_mutexattr_default ((pthread_mutexattr_t *)NULL)
#endif
#if !defined(pthread_condattr_default)
#  define pthread_condattr_default ((pthread_condattr_t *)NULL)
#endif


/* Whether or not to use semaphores directly rather than emulating them with
 * mutexes and condition variables:
 */
#if defined(_POSIX_SEMAPHORES) && !defined(HAVE_BROKEN_POSIX_SEMAPHORES)
#  define USE_SEMAPHORES
#else
#  undef USE_SEMAPHORES
#endif


/* On platforms that don't use standard POSIX threads pthread_sigmask()
 * isn't present.  DEC threads uses sigprocmask() instead as do most
 * other UNIX International compliant systems that don't have the full
 * pthread implementation.
 */
#if defined(HAVE_PTHREAD_SIGMASK) && !defined(HAVE_BROKEN_PTHREAD_SIGMASK)
#  define SET_THREAD_SIGMASK pthread_sigmask
#else
#  define SET_THREAD_SIGMASK sigprocmask
#endif


#if 0
/* A pthread mutex isn't sufficient to model the Python lock type
 * because, according to Draft 5 of the docs (P1003.4a/D5), both of the
 * following are undefined:
 *  -> a thread tries to lock a mutex it already has locked
 *  -> a thread tries to unlock a mutex locked by a different thread
 * pthread mutexes are designed for serializing threads over short pieces
 * of code anyway, so wouldn't be an appropriate implementation of
 * Python's locks regardless.
 *
 * The pthread_lock struct implements a Python lock as a "locked?" bit
 * and a <condition, mutex> pair.  In general, if the bit can be acquired
 * instantly, it is, else the pair is used to block the thread until the
 * bit is cleared.     9 May 1994 tim@ksr.com
 */

typedef struct {
	char             locked; /* 0=unlocked, 1=locked */
	/* a <cond, mutex> pair to handle an acquire of a locked lock */
	pthread_cond_t   lock_released;
	pthread_mutex_t  mut;
} pthread_lock;
#endif


struct _PyThread_type_lock {
    pthread_mutex_t mutex;
};

#ifdef USE_SEMAPHORES
struct _PyThread_type_sem {
    sem_t sem;
};
#else
struct _PyThread_type_sem {
    int available;
    pthread_cond_t released;
    pthread_mutex_t mutex;
};
#endif

struct _PyThread_type_cond {
    pthread_cond_t cond;
};

struct _PyThread_type_key {
    pthread_key_t key;
};

struct _PyThread_type_handle {
    /* Note, these are not comparable.  Compare PyStates instead. */
    pthread_t value;
};

struct _PyThread_type_timeout {
    struct timespec abstime;
    int expired;
};

struct _PyThread_type_flag {
    int value;
    int waiting;
    pthread_cond_t wakeup;
    pthread_mutex_t mutex;
};


#define CHECK_STATUS(name)  if (status != 0) { perror(name); error = 1; }
#define CHECK_STATUS_ABORT(name)  if (status != 0) { perror(name); abort(); }

/*
 * Initialization.
 */

#ifdef _HAVE_BSDI
static
void _noop(void)
{
}

static void
PyThread__init_thread(void)
{
	/* DO AN INIT BY STARTING THE THREAD */
	static int dummy = 0;
	pthread_t thread1;
	pthread_create(&thread1, NULL, (void *) _noop, &dummy);
	pthread_join(thread1, NULL);
}

#else /* !_HAVE_BSDI */

static void
PyThread__init_thread(void)
{
#if defined(_AIX) && defined(__GNUC__)
	pthread_init();
#endif
}

#endif /* !_HAVE_BSDI */

/*
 * Thread support.
 */


int
PyThread_start_new_thread(PyThread_type_handle **handle,
		void (*func)(void *), void *arg)
{
	pthread_t id;
	int status;
#if defined(THREAD_STACK_SIZE) || defined(PTHREAD_SYSTEM_SCHED_SUPPORTED)
	pthread_attr_t attrs;
#endif
#if defined(THREAD_STACK_SIZE)
	size_t tss;
#endif

	dprintf(("PyThread_start_new_thread called\n"));
	if (!initialized)
		PyThread_init_thread();

	if (handle != NULL) {
		*handle = malloc(sizeof(PyThread_type_handle));
		if (*handle == NULL)
			return -1;
	}

#if defined(THREAD_STACK_SIZE) || defined(PTHREAD_SYSTEM_SCHED_SUPPORTED)
	if (pthread_attr_init(&attrs) != 0)
		goto failed;
#endif
#if defined(THREAD_STACK_SIZE)
	tss = (_pythread_stacksize != 0) ? _pythread_stacksize
					 : THREAD_STACK_SIZE;
	if (tss != 0) {
		if (pthread_attr_setstacksize(&attrs, tss) != 0) {
			pthread_attr_destroy(&attrs);
			goto failed;
		}
	}
#endif
#if defined(PTHREAD_SYSTEM_SCHED_SUPPORTED)
        pthread_attr_setscope(&attrs, PTHREAD_SCOPE_SYSTEM);
#endif

	status = pthread_create(&id,
#if defined(THREAD_STACK_SIZE) || defined(PTHREAD_SYSTEM_SCHED_SUPPORTED)
				 &attrs,
#else
				 (pthread_attr_t*)NULL,
#endif
				 (void* (*)(void *))func,
				 (void *)arg
				 );

#if defined(THREAD_STACK_SIZE) || defined(PTHREAD_SYSTEM_SCHED_SUPPORTED)
	pthread_attr_destroy(&attrs);
#endif
	if (status != 0) {
		goto failed;
	}

	pthread_detach(id);

	if (handle != NULL)
		(*handle)->value = id;

	return 0;

failed:
	if (handle != NULL) {
		free(*handle);
		*handle = NULL;
	}
	return -1;
}

PyThread_type_handle *
PyThread_get_handle(void)
{
    PyThread_type_handle *handle;
    handle = malloc(sizeof(PyThread_type_handle));
    if (handle == NULL)
        return NULL;
    handle->value = pthread_self();
    return handle;
}

void
PyThread_free_handle(PyThread_type_handle *handle)
{
    assert(handle != NULL);
    free(handle);
}

void
PyThread_send_signal(PyThread_type_handle *handle, int signum)
{
    int status;

    assert(handle != NULL);

    status = pthread_kill(handle->value, signum);
    if (status < 0) {
        fprintf(stderr, "pthread_kill failed with %d\n", errno);
        Py_FatalError("PyThread_send_signal failed calling pthread_kill");
    }
}


/*
 * Lock support.
 */

PyThread_type_lock *
PyThread_lock_allocate(void)
{
	//sem_t *lock;
	PyThread_type_lock *lock;
	int status, error = 0;

	dprintf(("PyThread_allocate_lock called\n"));
	if (!initialized)
		PyThread_init_thread();

	//lock = (sem_t *)malloc(sizeof(sem_t));
	lock = malloc(sizeof(PyThread_type_lock));

	if (lock) {
		//status = sem_init(lock,0,1);
#if 0
		pthread_mutexattr_t attr;
		status = pthread_mutexattr_init(&attr);
		CHECK_STATUS_ABORT("pthread_mutexattr_init");

		status = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK_NP);
		CHECK_STATUS_ABORT("pthread_mutexattr_settype");

		pthread_mutex_init(&lock->mutex, &attr);
		CHECK_STATUS("pthread_mutex_init");

		status = pthread_mutexattr_destroy(&attr);
		CHECK_STATUS_ABORT("pthread_mutexattr_destroy");
#else
		status = pthread_mutex_init(&lock->mutex, NULL);
		CHECK_STATUS("pthread_mutex_init");
#endif

		if (error) {
			free(lock);
			lock = NULL;
		}
	}

	dprintf(("PyThread_allocate_lock() -> %p\n", lock));
	return lock;
}

void
PyThread_lock_free(PyThread_type_lock *lock)
{
	//sem_t *thelock = (sem_t *)lock;
	int status;

	dprintf(("PyThread_free_lock(%p) called\n", lock));

	assert(lock);

	//status = sem_destroy(thelock);
	status = pthread_mutex_destroy(&lock->mutex);
	CHECK_STATUS_ABORT("pthread_mutex_destroy");

	free(lock);
}

#include <execinfo.h>

void
PyThread_lock_acquire(PyThread_type_lock *lock)
{
	//sem_t *thelock = (sem_t *)lock;
	int status;
#if 0
	static unsigned long long count;

	count++;
	if ((count % 10000) == 0) {
		void *scratch[10] = {};
		printf("PyThread_lock_acquire: %llu\n", count);
		backtrace(scratch, 10);
		backtrace_symbols_fd(scratch, 2, 1);
		printf("*****\n");
	}
#endif

	dprintf(("PyThread_acquire_lock(%p) called\n", lock));

	//if (!waitflag)
	//	abort();
	status = pthread_mutex_lock(&lock->mutex);
	CHECK_STATUS_ABORT("pthread_mutex_lock");

	dprintf(("PyThread_acquire_lock(%p) done\n", lock));
}

/* Returns 1 on success, 0 on failure. */
int
PyThread_lock_tryacquire(PyThread_type_lock *lock)
{
	int success;
	//sem_t *thelock = (sem_t *)lock;
	int status;

	dprintf(("PyThread_tryacquire_lock(%p) called\n", lock));

	//if (!waitflag)
	//	abort();
	status = pthread_mutex_trylock(&lock->mutex);
	if (status != EBUSY)
		CHECK_STATUS_ABORT("pthread_mutex_trylock");

	success = (status == 0) ? 1 : 0;
	dprintf(("PyThread_tryacquire_lock(%p) done -> %d\n", lock, success));
	return success;
}

void
PyThread_lock_release(PyThread_type_lock *lock)
{
	//sem_t *thelock = (sem_t *)lock;
	int status;

	dprintf(("PyThread_release_lock(%p) called\n", lock));

	//status = sem_post(thelock);
	status = pthread_mutex_unlock(&lock->mutex);
	CHECK_STATUS_ABORT("pthread_mutex_unlock");

	dprintf(("PyThread_release_lock(%p) done\n", lock));
}


/*
 * Semaphore support.
 */

PyThread_type_sem *
PyThread_sem_allocate(int initial_value)
{
	PyThread_type_sem *sem;
	int status, error = 0;

	dprintf(("PyThread_sem_allocate called\n"));
	if (!initialized)
		PyThread_init_thread();

	if (initial_value < 0 || initial_value > 1)
		Py_FatalError("PyThread_sem_allocate given invalid initial_value");

	sem = malloc(sizeof(PyThread_type_sem));

	if (sem) {
#ifdef USE_SEMAPHORES
		status = sem_init(&sem->sem, 0, initial_value);
		CHECK_STATUS("sem_init");

		if (error) {
			free(sem);
			sem = NULL;
		}
#else
		sem->available = initial_value;
		status = pthread_cond_init(&sem->released, NULL);
		CHECK_STATUS("pthread_cond_init");

		if (error) {
			free(sem);
			sem = NULL;
		} else {
			status = pthread_mutex_init(&sem->mutex, NULL);
			CHECK_STATUS("pthread_mutex_init");

			if (error) {
				status = pthread_cond_destroy(&sem->released);
				CHECK_STATUS_ABORT("pthread_cond_destroy");
				free(sem);
				sem = NULL;
			}
		}
#endif
	}

	dprintf(("PyThread_sem_allocate() -> %p\n", sem));
	return sem;
}

void
PyThread_sem_free(PyThread_type_sem *sem)
{
	int status;

	dprintf(("PyThread_sem_free(%p) called\n", sem));

	assert(sem);

#ifdef USE_SEMAPHORES
	status = sem_destroy(&sem->sem);
	CHECK_STATUS_ABORT("sem_destroy");
#else
	status = pthread_cond_destroy(&sem->released);
	CHECK_STATUS_ABORT("pthread_cond_destroy");
	status = pthread_mutex_destroy(&sem->mutex);
	CHECK_STATUS_ABORT("pthread_mutex_destroy");
#endif

	free(sem);
}

#ifdef USE_SEMAPHORES
/*
 * As of February 2002, Cygwin thread implementations mistakenly report error
 * codes in the return value of the sem_ calls (like the pthread_ functions).
 * Correct implementations return -1 and put the code in errno. This supports
 * either.
 */
static int
fix_sem_status(int status)
{
	return (status == -1) ? errno : status;
}
#endif

void
PyThread_sem_acquire(PyThread_type_sem *sem)
{
	int status;

	dprintf(("PyThread_sem_acquire(%p) called\n", sem));

#ifdef USE_SEMAPHORES
	do {
		status = fix_sem_status(sem_wait(&sem->sem));
	} while (status == EINTR); /* Retry if interrupted by a signal */

	if (status != EINTR)
		CHECK_STATUS_ABORT("sem_wait");
#else
	status = pthread_mutex_lock(&sem->mutex);
	CHECK_STATUS_ABORT("pthread_mutex_lock");

	while (!sem->available) {
		status = pthread_cond_wait(&sem->released, &sem->mutex);
		CHECK_STATUS_ABORT("pthread_cond_wait");
	}

	sem->available = 0;
	status = pthread_mutex_unlock(&sem->mutex);
	CHECK_STATUS_ABORT("pthread_mutex_unlock");
#endif

	dprintf(("PyThread_sem_acquire(%p)\n", sem));
}

void
PyThread_sem_release(PyThread_type_sem *sem)
{
	int status;
#ifdef USE_SEMAPHORES
	int value = 0;
#endif

	dprintf(("PyThread_sem_release(%p) called\n", sem));

#ifdef USE_SEMAPHORES
	status = sem_getvalue(&sem->sem, &value);
	CHECK_STATUS_ABORT("sem_getvalue");
	if (value >= 1)
		Py_FatalError("PyThread_sem_release may not increase the value beyond 1");
	/* XXX There is a race here, but since one path is a FatalError
	 * anyway it's not a big deal. */

	status = sem_post(&sem->sem);
	CHECK_STATUS_ABORT("sem_post");
#else
	status = pthread_mutex_lock(&sem->mutex);
	CHECK_STATUS_ABORT("pthread_mutex_lock");

	if (sem->available)
		Py_FatalError("PyThread_sem_release may not increase the value beyond 1");

	status = pthread_cond_signal(&sem->released);
	CHECK_STATUS_ABORT("pthread_cond_signal");

	sem->available = 1;
	status = pthread_mutex_unlock(&sem->mutex);
	CHECK_STATUS_ABORT("pthread_mutex_unlock");
#endif
}


/*
 * Condition support.
 */

PyThread_type_cond *
PyThread_cond_allocate(void)
{
	PyThread_type_cond *cond;
	int status, error = 0;

	dprintf(("PyThread_cond_allocate called\n"));
	if (!initialized)
		PyThread_init_thread();

	cond = malloc(sizeof(PyThread_type_cond));

	if (cond) {
		status = pthread_cond_init(&cond->cond, NULL);
		CHECK_STATUS("pthread_cond_init");

		if (error) {
			free(cond);
			cond = NULL;
		}
	}

	dprintf(("PyThread_cond_allocate() -> %p\n", cond));
	return cond;
}

void
PyThread_cond_free(PyThread_type_cond *cond)
{
	int status;

	dprintf(("PyThread_cond_free(%p) called\n", cond));

	assert(cond);

	status = pthread_cond_destroy(&cond->cond);
	CHECK_STATUS_ABORT("pthread_cond_destroy");

	free(cond);
}

void
PyThread_cond_wait(PyThread_type_cond *cond, PyThread_type_lock *lock)
{
	int status;

	dprintf(("PyThread_cond_wait(%p, %p) called\n", cond, lock));

	status = pthread_cond_wait(&cond->cond, &lock->mutex);
	CHECK_STATUS_ABORT("pthread_cond_wait");
	
	dprintf(("PyThread_cond_wait(%p, %p) done\n", cond, lock));
}

void
PyThread_cond_timedwait(PyThread_type_cond *cond, PyThread_type_lock *lock,
        PyThread_type_timeout *timeout)
{
    int status;

    status = pthread_cond_timedwait(&cond->cond, &lock->mutex, &timeout->abstime);
    if (status == ETIMEDOUT)
        timeout->expired = 1;
    else
        CHECK_STATUS_ABORT("pthread_cond_timedwait");
}

void
PyThread_cond_wakeone(PyThread_type_cond *cond)
{
	int status;

	dprintf(("PyThread_cond_wakeone(%p) called\n", cond));

	status = pthread_cond_signal(&cond->cond);
	CHECK_STATUS_ABORT("pthread_cond_signal");
	
	dprintf(("PyThread_cond_wakeone(%p) done\n", cond));
}

void
PyThread_cond_wakeall(PyThread_type_cond *cond)
{
	int status;

	dprintf(("PyThread_cond_wakeall(%p) called\n", cond));

	status = pthread_cond_broadcast(&cond->cond);
	CHECK_STATUS_ABORT("pthread_cond_broadcast");
	
	dprintf(("PyThread_cond_wakeall(%p) done\n", cond));
}


/*
 * Thread-local Storage support.
 */

#define Py_HAVE_NATIVE_TLS

PyThread_type_key *
PyThread_create_key(void)
{
	PyThread_type_key *key;
	int status, error = 0;

	key = malloc(sizeof(PyThread_type_key));

	if (key) {
		//status = sem_init(lock,0,1);
		//status = pthread_mutex_init(lock, NULL);
		status = pthread_key_create(&key->key, NULL);
		CHECK_STATUS("pthread_key_create");

		if (error) {
			free(key);
			key = NULL;
		}
	}

	return key;
}

void
PyThread_delete_key(PyThread_type_key *key)
{
	int status = 0;

	status = pthread_key_delete(key->key);
	CHECK_STATUS_ABORT("pthread_key_delete");
}

/* Unlock the default implementation, I consider replacing an existing
 * key to be an error.  I'm not going to check it. */
void
PyThread_set_key_value(PyThread_type_key *key, void *value)
{
	int status = 0;

	assert(key != NULL); /* Use PyThread_delete_key_value to delete */
	status = pthread_setspecific(key->key, value);
	CHECK_STATUS_ABORT("pthread_setspecific");
}

void *
PyThread_get_key_value(PyThread_type_key *key)
{
	return pthread_getspecific(key->key);
}

void
PyThread_delete_key_value(PyThread_type_key *key)
{
	int status = 0;

	status = pthread_setspecific(key->key, NULL);
	CHECK_STATUS_ABORT("pthread_setspecific");
}


/* set the thread stack size.
 * Return 0 if size is valid, -1 if size is invalid,
 * -2 if setting stack size is not supported.
 */
static int
_pythread_pthread_set_stacksize(size_t size)
{
#if defined(THREAD_STACK_SIZE)
	pthread_attr_t attrs;
	size_t tss_min;
	int rc = 0;
#endif

	/* set to default */
	if (size == 0) {
		_pythread_stacksize = 0;
		return 0;
	}

#if defined(THREAD_STACK_SIZE)
#if defined(PTHREAD_STACK_MIN)
	tss_min = PTHREAD_STACK_MIN > THREAD_STACK_MIN ? PTHREAD_STACK_MIN
						       : THREAD_STACK_MIN;
#else
	tss_min = THREAD_STACK_MIN;
#endif
	if (size >= tss_min) {
		/* validate stack size by setting thread attribute */
		if (pthread_attr_init(&attrs) == 0) {
			rc = pthread_attr_setstacksize(&attrs, size);
			pthread_attr_destroy(&attrs);
			if (rc == 0) {
				_pythread_stacksize = size;
				return 0;
			}
		}
	}
	return -1;
#else
	return -2;
#endif
}

#define THREAD_SET_STACKSIZE(x)	_pythread_pthread_set_stacksize(x)


PyThread_type_timeout *
PyThread_timeout_allocate(void)
{
    PyThread_type_timeout *timeout;

    timeout = malloc(sizeof(PyThread_type_timeout));
    if (timeout == NULL)
        return NULL;

    timeout->abstime.tv_sec = 0;
    timeout->abstime.tv_nsec = 0;
    timeout->expired = 1;

    return timeout;
}

void
PyThread_timeout_free(PyThread_type_timeout *timeout)
{
    assert(timeout);
    free(timeout);
}

#define BOUND(low, value, high) ((low) >= (value) ? (low) : (high) <= (value) ? (high) : (value))
#define MAX_DELAY (60*60*24*365)

static void
timeout_convertdelay(struct timespec *abstime, double delay)
{
    int status;
    struct timeval tv;
    double frac, integral;
    time_t sec;
    long nsec;

    status = gettimeofday(&tv, NULL);
    CHECK_STATUS_ABORT("gettimeofday");

    if (delay <= 0.0)
        delay = 0.0;
    else if (delay >= MAX_DELAY)
        /* Ensure no overflows until at least 2037.  By then you should
         * be using at least 64-bit anyway. */
        delay = MAX_DELAY;

    frac = modf(delay, &integral);
    sec = (time_t)integral;
    nsec = BOUND(0, (long)(frac*1000000000), 999999999);

    abstime->tv_sec = tv.tv_sec + sec;
    abstime->tv_nsec = tv.tv_usec * 1000 + nsec;
    if (abstime->tv_nsec >= 1000000000) {
        abstime->tv_sec += 1;
        abstime->tv_nsec -= 1000000000;
    }
}

void
PyThread_timeout_set(PyThread_type_timeout *timeout, double delay)
{
    timeout_convertdelay(&timeout->abstime, delay);
    timeout->expired = 0;
}

/* Note that a timeout is only set to expired when it is used.  This
 * function does *not* check the current time. */
int
PyThread_timeout_expired(PyThread_type_timeout *timeout)
{
    return timeout->expired;
}


PyThread_type_flag *
PyThread_flag_allocate(void)
{
    PyThread_type_flag *flag;
    int status, error = 0;

    flag = malloc(sizeof(PyThread_type_flag));
    if (flag == NULL)
        return NULL;

    flag->value = 0;
    flag->waiting = 0;
    status = pthread_cond_init(&flag->wakeup, NULL);
    CHECK_STATUS("pthread_cond_init");

    if (error) {
            free(flag);
            flag = NULL;
    } else {
            status = pthread_mutex_init(&flag->mutex, NULL);
            CHECK_STATUS("pthread_mutex_init");

            if (error) {
                    status = pthread_cond_destroy(&flag->wakeup);
                    CHECK_STATUS_ABORT("pthread_cond_destroy");
                    free(flag);
                    flag = NULL;
            }
    }

    return flag;
}

void
PyThread_flag_free(PyThread_type_flag *flag)
{
    int status;

    assert(flag);
    status = pthread_cond_destroy(&flag->wakeup);
    CHECK_STATUS_ABORT("pthread_cond_destroy");
    status = pthread_mutex_destroy(&flag->mutex);
    CHECK_STATUS_ABORT("pthread_mutex_destroy");
    free(flag);
}

void
PyThread_flag_set(PyThread_type_flag *flag)
{
    int status;

    status = pthread_mutex_lock(&flag->mutex);
    CHECK_STATUS_ABORT("pthread_mutex_lock");

    if (!flag->value && flag->waiting) {
        status = pthread_cond_signal(&flag->wakeup);
        CHECK_STATUS_ABORT("pthread_cond_signal");
    }
    flag->value = 1;

    status = pthread_mutex_unlock(&flag->mutex);
    CHECK_STATUS_ABORT("pthread_mutex_unlock");
}

void
PyThread_flag_clear(PyThread_type_flag *flag)
{
    int status;

    status = pthread_mutex_lock(&flag->mutex);
    CHECK_STATUS_ABORT("pthread_mutex_lock");

    if (flag->waiting)
        Py_FatalError("A flag cannoted be cleared while a thread is waiting");

    flag->value = 0;

    status = pthread_mutex_unlock(&flag->mutex);
    CHECK_STATUS_ABORT("pthread_mutex_unlock");
}

void
PyThread_flag_wait(PyThread_type_flag *flag)
{
    int status;

    status = pthread_mutex_lock(&flag->mutex);
    CHECK_STATUS_ABORT("pthread_mutex_lock");

    if (flag->waiting)
        Py_FatalError("Only one thread may wait on a flag");
    flag->waiting = 1;

    while (!flag->value) {
            status = pthread_cond_wait(&flag->wakeup, &flag->mutex);
            CHECK_STATUS_ABORT("pthread_cond_wait");
    }

    flag->waiting = 0;

    status = pthread_mutex_unlock(&flag->mutex);
    CHECK_STATUS_ABORT("pthread_mutex_unlock");
}

int
PyThread_flag_timedwait(PyThread_type_flag *flag, double delay)
{
    struct timespec abstime;
    int status, value;

    timeout_convertdelay(&abstime, delay);

    status = pthread_mutex_lock(&flag->mutex);
    CHECK_STATUS_ABORT("pthread_mutex_lock");

    if (flag->waiting)
        Py_FatalError("Only one thread may wait on a flag");
    flag->waiting = 1;

    while (!flag->value) {
            status = pthread_cond_timedwait(&flag->wakeup, &flag->mutex,
                &abstime);
            if (status == ETIMEDOUT)
                break;
            else
                CHECK_STATUS_ABORT("pthread_cond_wait");
    }

    value = flag->value;
    flag->waiting = 0;

    status = pthread_mutex_unlock(&flag->mutex);
    CHECK_STATUS_ABORT("pthread_mutex_unlock");

    return value;
}
