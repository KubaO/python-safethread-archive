
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
PyThread_start_new_thread(PyThread_type_handle *handle, void (*func)(void *), void *arg)
{
	PyThread_type_handle dummy_handle;
	int status;
#if defined(THREAD_STACK_SIZE) || defined(PTHREAD_SYSTEM_SCHED_SUPPORTED)
	pthread_attr_t attrs;
#endif
#if defined(THREAD_STACK_SIZE)
	size_t tss;
#endif

	if (handle == NULL)
		handle = &dummy_handle;

	dprintf(("PyThread_start_new_thread called\n"));
	if (!initialized)
		PyThread_init_thread();

#if defined(THREAD_STACK_SIZE) || defined(PTHREAD_SYSTEM_SCHED_SUPPORTED)
	if (pthread_attr_init(&attrs) != 0)
		return -1;
#endif
#if defined(THREAD_STACK_SIZE)
	tss = (_pythread_stacksize != 0) ? _pythread_stacksize
					 : THREAD_STACK_SIZE;
	if (tss != 0) {
		if (pthread_attr_setstacksize(&attrs, tss) != 0) {
			pthread_attr_destroy(&attrs);
			return -1;
		}
	}
#endif
#if defined(PTHREAD_SYSTEM_SCHED_SUPPORTED)
        pthread_attr_setscope(&attrs, PTHREAD_SCOPE_SYSTEM);
#endif

	status = pthread_create(&handle->_value,
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
	if (status != 0)
            return -1;

        pthread_detach(handle->_value);

        return 0;
}

PyThread_type_handle
PyThread_get_handle(void)
{
    PyThread_type_handle handle;
    handle._value = pthread_self();
    return handle;
}

void
PyThread_send_signal(PyThread_type_handle handle, int signum)
{
    int status = pthread_kill(handle._value, signum);
    if (status < 0) {
        fprintf(stderr, "pthread_kill failed with %d\n", errno);
        Py_FatalError("PyThread_send_signal failed calling pthread_kill");
    }
}


/*
 * Lock support.
 */

PyThread_type_lock
PyThread_lock_allocate(void)
{
	//sem_t *lock;
	pthread_mutex_t *lock;
	int status, error = 0;

	dprintf(("PyThread_allocate_lock called\n"));
	if (!initialized)
		PyThread_init_thread();

	//lock = (sem_t *)malloc(sizeof(sem_t));
	lock = malloc(sizeof(pthread_mutex_t));

	if (lock) {
		//status = sem_init(lock,0,1);
#if 0
		pthread_mutexattr_t attr;
		status = pthread_mutexattr_init(&attr);
		CHECK_STATUS_ABORT("pthread_mutexattr_init");

		status = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK_NP);
		CHECK_STATUS_ABORT("pthread_mutexattr_settype");

		pthread_mutex_init(lock, &attr);
		CHECK_STATUS("pthread_mutex_init");

		status = pthread_mutexattr_destroy(&attr);
		CHECK_STATUS_ABORT("pthread_mutexattr_destroy");
#else
		status = pthread_mutex_init(lock, NULL);
		CHECK_STATUS("pthread_mutex_init");
#endif

		if (error) {
			free((void *)lock);
			lock = NULL;
		}
	}

	dprintf(("PyThread_allocate_lock() -> %p\n", lock));
	return (PyThread_type_lock)lock;
}

void
PyThread_lock_free(PyThread_type_lock lock)
{
	//sem_t *thelock = (sem_t *)lock;
	pthread_mutex_t *thelock = (pthread_mutex_t *)lock;
	int status;

	dprintf(("PyThread_free_lock(%p) called\n", lock));

	if (!thelock)
		return;

	//status = sem_destroy(thelock);
	status = pthread_mutex_destroy(thelock);
	CHECK_STATUS_ABORT("pthread_mutex_destroy");

	free((void *)thelock);
}

#include <execinfo.h>

void
PyThread_lock_acquire(PyThread_type_lock lock)
{
	//sem_t *thelock = (sem_t *)lock;
	pthread_mutex_t *thelock = (pthread_mutex_t *)lock;
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
	status = pthread_mutex_lock(thelock);
	CHECK_STATUS_ABORT("pthread_mutex_lock");

	dprintf(("PyThread_acquire_lock(%p) done\n", lock));
}

/* This is only temporary, until a better interface is available to those
 * who need such functionality. */
int
_PyThread_lock_tryacquire(PyThread_type_lock lock)
{
	int success;
	//sem_t *thelock = (sem_t *)lock;
	pthread_mutex_t *thelock = (pthread_mutex_t *)lock;
	int status;

	dprintf(("PyThread_tryacquire_lock(%p) called\n", lock));

	//if (!waitflag)
	//	abort();
	status = pthread_mutex_trylock(thelock);
	if (status != EBUSY)
		CHECK_STATUS_ABORT("pthread_mutex_trylock");

	success = (status == 0) ? 1 : 0;
	dprintf(("PyThread_tryacquire_lock(%p) done -> %d\n", lock, success));
	return success;
}

void
PyThread_lock_release(PyThread_type_lock lock)
{
	//sem_t *thelock = (sem_t *)lock;
	pthread_mutex_t *thelock = (pthread_mutex_t *)lock;
	int status;

	dprintf(("PyThread_release_lock(%p) called\n", lock));

	//status = sem_post(thelock);
	status = pthread_mutex_unlock(thelock);
	CHECK_STATUS_ABORT("pthread_mutex_unlock");

	dprintf(("PyThread_release_lock(%p) done\n", lock));
}


/*
 * Semaphore support.
 */

PyThread_type_sem
PyThread_sem_allocate(int initial_value)
{
	sem_t *sem;
	int status, error = 0;

	dprintf(("PyThread_sem_allocate called\n"));
	if (!initialized)
		PyThread_init_thread();

	if (initial_value < 0 || initial_value > 1)
		Py_FatalError("PyThread_sem_allocate given invalid initial_value");

	sem = malloc(sizeof(sem_t));

	if (sem) {
		status = sem_init(sem, 0, initial_value);
		CHECK_STATUS("sem_init");

		if (error) {
			free((void *)sem);
			sem = NULL;
		}
	}

	dprintf(("PyThread_sem_allocate() -> %p\n", sem));
	return (PyThread_type_sem)sem;
}

void
PyThread_sem_free(PyThread_type_sem sem)
{
	sem_t *thesem = (sem_t *)sem;
	int status;

	dprintf(("PyThread_sem_free(%p) called\n", sem));

	if (!thesem)
		return;

	status = sem_destroy(thesem);
	CHECK_STATUS_ABORT("sem_destroy");

	free((void *)thesem);
}

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

void
PyThread_sem_acquire(PyThread_type_sem sem)
{
	sem_t *thesem = (sem_t *)sem;
	int status;

	dprintf(("PyThread_sem_acquire(%p) called\n", sem));

	do {
		status = fix_sem_status(sem_wait(thesem));
	} while (status == EINTR); /* Retry if interrupted by a signal */

	if (status != EINTR)
		CHECK_STATUS_ABORT("sem_wait");
	
	dprintf(("PyThread_sem_acquire(%p)\n", sem));
}

void
PyThread_sem_release(PyThread_type_sem sem)
{
	sem_t *thesem = (sem_t *)sem;
	int status, value = 0;

	dprintf(("PyThread_sem_release(%p) called\n", sem));

	status = sem_getvalue(thesem, &value);
	CHECK_STATUS_ABORT("sem_getvalue");
	if (value >= 1)
		Py_FatalError("PyThread_sem_release may not increase the value beyond 1");
	/* XXX There is a race here, but since one path is a FatalError
	 * anyway it's not a big deal. */

	status = sem_post(thesem);
	CHECK_STATUS_ABORT("sem_post");
}


/*
 * Condition support.
 */

PyThread_type_cond
PyThread_cond_allocate(void)
{
	pthread_cond_t *cond;
	int status, error = 0;

	dprintf(("PyThread_cond_allocate called\n"));
	if (!initialized)
		PyThread_init_thread();

	cond = malloc(sizeof(pthread_cond_t));

	if (cond) {
		status = pthread_cond_init(cond, NULL);
		CHECK_STATUS("pthread_cond_init");

		if (error) {
			free((void *)cond);
			cond = NULL;
		}
	}

	dprintf(("PyThread_cond_allocate() -> %p\n", cond));
	return (PyThread_type_cond)cond;
}

void
PyThread_cond_free(PyThread_type_cond cond)
{
	pthread_cond_t *thecond = (pthread_cond_t *)cond;
	int status;

	dprintf(("PyThread_cond_free(%p) called\n", cond));

	if (!thecond)
		return;

	status = pthread_cond_destroy(thecond);
	CHECK_STATUS_ABORT("pthread_cond_destroy");

	free((void *)thecond);
}

void
PyThread_cond_wait(PyThread_type_cond cond, PyThread_type_lock lock)
{
	pthread_cond_t *thecond = (pthread_cond_t *)cond;
	pthread_mutex_t *thelock = (pthread_mutex_t *)lock;
	int status;

	dprintf(("PyThread_cond_wait(%p, %p) called\n", cond, lock));

	status = pthread_cond_wait(thecond, thelock);
	CHECK_STATUS_ABORT("pthread_cond_wait");
	
	dprintf(("PyThread_cond_wait(%p, %p) done\n", cond, lock));
}

void
PyThread_cond_wakeone(PyThread_type_cond cond)
{
	pthread_cond_t *thecond = (pthread_cond_t *)cond;
	int status;

	dprintf(("PyThread_cond_wakeone(%p) called\n", cond));

	status = pthread_cond_signal(thecond);
	CHECK_STATUS_ABORT("pthread_cond_signal");
	
	dprintf(("PyThread_cond_wakeone(%p) done\n", cond));
}

void
PyThread_cond_wakeall(PyThread_type_cond cond)
{
	pthread_cond_t *thecond = (pthread_cond_t *)cond;
	int status;

	dprintf(("PyThread_cond_wakeall(%p) called\n", cond));

	status = pthread_cond_broadcast(thecond);
	CHECK_STATUS_ABORT("pthread_cond_broadcast");
	
	dprintf(("PyThread_cond_wakeall(%p) done\n", cond));
}


/*
 * Thread-local Storage support.
 */

#define Py_HAVE_NATIVE_TLS

PyThread_type_key
PyThread_create_key(void)
{
	pthread_key_t *thekey;
	int status, error = 0;

	thekey = malloc(sizeof(pthread_key_t));

	if (thekey) {
		//status = sem_init(lock,0,1);
		//status = pthread_mutex_init(lock, NULL);
		status = pthread_key_create(thekey, NULL);
		CHECK_STATUS("pthread_key_create");

		if (error) {
			free((void *)thekey);
			thekey = NULL;
		}
	}

	return (PyThread_type_key *)thekey;
}

void
PyThread_delete_key(PyThread_type_key key)
{
	pthread_key_t *thekey = (pthread_key_t *)key;
	int status = 0;

	status = pthread_key_delete(*thekey);
	CHECK_STATUS_ABORT("pthread_key_delete");
}

/* Unlock the default implementation, I consider replacing an existing
 * key to be an error.  I'm not going to check it. */
void
PyThread_set_key_value(PyThread_type_key key, void *value)
{
	pthread_key_t *thekey = (pthread_key_t *)key;
	int status = 0;

	assert(thekey != NULL); /* Use PyThread_delete_key_value to delete */
	status = pthread_setspecific(*thekey, value);
	CHECK_STATUS_ABORT("pthread_setspecific");
}

void *
PyThread_get_key_value(PyThread_type_key key)
{
	pthread_key_t *thekey = (pthread_key_t *)key;
	return pthread_getspecific(*thekey);
}

void
PyThread_delete_key_value(PyThread_type_key key)
{
	pthread_key_t *thekey = (pthread_key_t *)key;
	int status = 0;

	status = pthread_setspecific(*thekey, NULL);
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
