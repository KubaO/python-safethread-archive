
/* Thread and interpreter state structures and their interfaces */

#include "Python.h"
#include "monitorobject.h"
#include "interruptobject.h"

/* --------------------------------------------------------------------------
CAUTION

Always use malloc() and free() directly in this file.  A number of these
functions are advertised as safe to call when the GIL isn't held, and in
a debug build Python redirects (e.g.) PyMem_NEW (etc) to Python's debugging
obmalloc functions.  Those aren't thread-safe (they rely on the GIL to avoid
the expense of doing their own locking).
-------------------------------------------------------------------------- */

#ifdef HAVE_DLOPEN
#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif
#ifndef RTLD_LAZY
#define RTLD_LAZY 1
#endif
#endif


#include "pythread.h"
static PyThread_type_lock head_mutex = NULL; /* Protects interp->tstate_head */
#define HEAD_INIT() (void)(head_mutex || (head_mutex = PyThread_lock_allocate()))
#define HEAD_LOCK() PyThread_lock_acquire(head_mutex)
#define HEAD_UNLOCK() PyThread_lock_release(head_mutex)

#ifdef __cplusplus
extern "C" {
#endif

void _PyMonitorSpace_Push(PyMonitorSpaceFrame *frame, struct _PyMonitorSpaceObject *monitorspace);
void _PyMonitorSpace_Pop(PyMonitorSpaceFrame *frame);

/* The single PyInterpreterState used by this process'
   GILState implementation
*/
static PyInterpreterState *autoInterpreterState = NULL;
static PyThread_type_key autoTLSkey = 0;

static PyInterpreterState *interp_head = NULL;

/* This hook exists so psyco can provide it's own frame objects */
static struct _frame *threadstate_getframe(PyThreadState *self);
PyThreadFrameGetter _PyThreadState_GetFrame = threadstate_getframe;

typedef struct _pending_writer {
	struct _pending_writer *next;
	PyThread_type_sem sem;
} pending_writer;

static PyThread_type_lock interpreter_lock = NULL; /* This is the GIL */
static PyThread_type_cond pending_readers;
static PyThread_type_cond active_readers;
static Py_ssize_t pending_readers_count;
static Py_ssize_t active_readers_count;
static int active_writer;
static pending_writer *pending_writers;
static pending_writer *pending_writers_last;
static long main_thread = 0;

__thread PyThreadState *_py_local_tstate;


PyInterpreterState *
PyInterpreterState_New(void)
{
	PyInterpreterState *interp;

	if (autoInterpreterState)
		Py_FatalError("InterpreterState already exists");

	interp = malloc(sizeof(PyInterpreterState));

	if (interp != NULL) {
		HEAD_INIT();
		if (head_mutex == NULL)
			Py_FatalError("Can't initialize threads for interpreter");
		interp->modules = NULL;
		interp->modules_reloading = NULL;
		interp->sysdict = NULL;
		interp->builtins = NULL;
		interp->tstate_head = NULL;
		interp->tstate_count = 0;
		interp->entertag = 0;
		interp->codec_search_path = NULL;
		interp->codec_search_cache = NULL;
		interp->codec_error_registry = NULL;
#ifdef HAVE_DLOPEN
#ifdef RTLD_NOW
                interp->dlopenflags = RTLD_NOW;
#else
		interp->dlopenflags = RTLD_LAZY;
#endif
#endif
#ifdef WITH_TSC
		interp->tscdump = 0;
#endif

		HEAD_LOCK();
		interp->next = interp_head;
		interp_head = interp;
		HEAD_UNLOCK();

		autoInterpreterState = interp;
	}

	return interp;
}


void
PyInterpreterState_Clear(PyInterpreterState *interp)
{
	PyThreadState *p;
	HEAD_LOCK();
	for (p = interp->tstate_head; p != NULL; p = p->next)
		PyThreadState_Clear(p);
	HEAD_UNLOCK();
	Py_CLEAR(interp->codec_search_path);
	Py_CLEAR(interp->codec_search_cache);
	Py_CLEAR(interp->codec_error_registry);
	Py_CLEAR(interp->modules);
	Py_CLEAR(interp->modules_reloading);
	Py_CLEAR(interp->sysdict);
	Py_CLEAR(interp->builtins);
}


void
PyInterpreterState_Delete(PyInterpreterState *interp)
{
	PyInterpreterState **p;
	if (AO_load_full(&interp->tstate_count) != 0)
		Py_FatalError("Attempting to delete PyInterpreterState with threads left");
	HEAD_LOCK();
	for (p = &interp_head; ; p = &(*p)->next) {
		if (*p == NULL)
			Py_FatalError(
				"PyInterpreterState_Delete: invalid interp");
		if (*p == interp)
			break;
	}
	if (interp->tstate_head != NULL)
		Py_FatalError("PyInterpreterState_Delete: remaining threads");
	*p = interp->next;
	HEAD_UNLOCK();
	free(interp);
	autoInterpreterState = NULL;
}


/* Default implementation for _PyThreadState_GetFrame */
static struct _frame *
threadstate_getframe(PyThreadState *self)
{
	return self->frame;
}

PyThreadState *
_PyThreadState_New(void)
{
	PyThreadState *tstate;
	PyObject *monitorspace;
	int i, j;

	tstate = malloc(sizeof(PyThreadState));
	if (tstate == NULL)
		return NULL;

	tstate->interp = NULL;

	tstate->frame = NULL;
	tstate->recursion_depth = 0;
	tstate->overflowed = 0;
	tstate->recursion_critical = 0;
	tstate->tracing = 0;
	tstate->use_tracing = 0;

	tstate->large_ticks = 0;
	tstate->small_ticks = 0;

	tstate->inspect_count = 0;
	tstate->inspect_queue_lock = NULL;
	tstate->inspect_lock = NULL;
	tstate->inspect_flag = 0;

	tstate->suspended = 1;

	tstate->enterframe = NULL;

	tstate->async_exc = NULL;
	tstate->thread_id = 0;

	tstate->dict = NULL;

	tstate->curexc_type = NULL;
	tstate->curexc_value = NULL;
	tstate->curexc_traceback = NULL;

	tstate->exc_type = NULL;
	tstate->exc_value = NULL;
	tstate->exc_traceback = NULL;

	tstate->c_profilefunc = NULL;
	tstate->c_tracefunc = NULL;
	tstate->c_profileobj = NULL;
	tstate->c_traceobj = NULL;

	tstate->import_depth = 0;
	tstate->monitorspace_frame = &tstate->_base_monitorspace_frame;
	tstate->_base_monitorspace_frame.prevframe = NULL;
	tstate->_base_monitorspace_frame.monitorspace = NULL;

	for (i = 0; i < PYMALLOC_CACHE_SIZECLASSES; i++) {
		for (j = 0; j < PYMALLOC_CACHE_COUNT; j++)
			tstate->malloc_cache[i][j] = NULL;
	}

	for (i = 0; i < PYGC_CACHE_SIZECLASSES; i++) {
		for (j = 0; j < PYGC_CACHE_COUNT; j++)
			tstate->gc_object_cache[i][j] = NULL;
	}

	for (i = 0; i < Py_ASYNCREFCOUNT_TABLE; i ++) {
		tstate->async_refcounts[i].obj = NULL;
		tstate->async_refcounts[i].diff = 0;
	}

	tstate->interrupt_point = NULL;

	tstate->critical_section = NULL;

	tstate->active_lock = NULL;
	tstate->lockwait_prev = NULL;
	tstate->lockwait_next = NULL;
	tstate->lockwait_cond = NULL;

	tstate->lockwait_cond = PyThread_cond_allocate();
	if (!tstate->lockwait_cond)
		goto failed;
	tstate->inspect_queue_lock = PyThread_lock_allocate();
	if (!tstate->inspect_queue_lock)
		goto failed;
	tstate->inspect_lock = PyThread_lock_allocate();
	if (!tstate->inspect_lock)
		goto failed;

	//printf("New tstate %p\n", tstate);
	return tstate;

failed:
	if (tstate->lockwait_cond)
		PyThread_cond_free(tstate->lockwait_cond);
	if (tstate->inspect_queue_lock)
		PyThread_lock_free(tstate->inspect_queue_lock);
	if (tstate->inspect_lock)
		PyThread_lock_free(tstate->inspect_lock);
	free(tstate);
	return NULL;
}

static void
_PyThreadState_Bind(PyInterpreterState *interp, PyThreadState *tstate)
{
	assert(autoTLSkey);
	if (PyThread_get_key_value(autoTLSkey) != 0)
		Py_FatalError("Thread already has PyThreadState");

	tstate->interp = interp;
	tstate->thread_id = PyThread_get_thread_ident();
	PyThread_set_key_value(autoTLSkey, tstate);
	_py_local_tstate = tstate;

	AO_fetch_and_add1_full(&interp->tstate_count);

	HEAD_LOCK();
	tstate->next = interp->tstate_head;
	interp->tstate_head = tstate;
	HEAD_UNLOCK();
}

void
PyThreadState_Clear(PyThreadState *tstate)
{
	if (Py_VerboseFlag && tstate->frame != NULL)
		fprintf(stderr,
		  "PyThreadState_Clear: warning: thread still has a frame\n");
	if (Py_VerboseFlag && tstate->monitorspace_frame != &tstate->_base_monitorspace_frame)
		fprintf(stderr,
		  "PyThreadState_Clear: warning: thread still has monitorspace frame\n");

	Py_CLEAR(tstate->frame);

	Py_CLEAR(tstate->dict);
	Py_CLEAR(tstate->async_exc);

	Py_CLEAR(tstate->curexc_type);
	Py_CLEAR(tstate->curexc_value);
	Py_CLEAR(tstate->curexc_traceback);

	Py_CLEAR(tstate->exc_type);
	Py_CLEAR(tstate->exc_value);
	Py_CLEAR(tstate->exc_traceback);

	tstate->c_profilefunc = NULL;
	tstate->c_tracefunc = NULL;
	Py_CLEAR(tstate->c_profileobj);
	Py_CLEAR(tstate->c_traceobj);
}

void
_PyThreadState_Delete(PyThreadState *tstate)
{
	assert(tstate->interp == NULL);
	assert(tstate->thread_id == 0);
	assert(tstate->critical_section == NULL);

	//printf("Deleting tstate %p\n", tstate);
	//free(tstate);
	/* XXX FIXME We need a users count and we need to delay actual
	 * deletion until the tracing GC goes through and resets the
	 * owner fields. */
	/* We're also leaking locks and stuff */
}

static void
_PyThreadState_Unbind(PyThreadState *tstate)
{
	PyInterpreterState *interp;
	PyThreadState **p;

	assert(tstate != NULL && tstate == PyThreadState_Get());
	_PyGC_Object_Cache_Flush();
	_PyGC_AsyncRefcount_Flush();
	PyState_Suspend();

	if (tstate == NULL)
		Py_FatalError("PyThreadState_Delete: NULL tstate");
	interp = tstate->interp;
	if (interp == NULL)
		Py_FatalError("PyThreadState_Delete: NULL interp");

	assert(tstate->interrupt_point == NULL);
	assert(tstate->active_lock == NULL);
	PyThread_cond_free(tstate->lockwait_cond);

	HEAD_LOCK();
	for (p = &interp->tstate_head; ; p = &(*p)->next) {
		if (*p == NULL)
			Py_FatalError(
				"PyThreadState_Delete: invalid tstate");
		if (*p == tstate)
			break;
	}
	*p = tstate->next;
	HEAD_UNLOCK();

	AO_fetch_and_sub1_full(&tstate->interp->tstate_count);

	PyThread_delete_key_value(autoTLSkey);
	_py_local_tstate = NULL;

	tstate->interp = NULL;
	tstate->thread_id = 0;
}


PyThreadState *
_PyThreadState_Get(void)
{
	//PyThreadState *tstate = PyThread_get_key_value(autoTLSkey);
	PyThreadState *tstate = _py_local_tstate;
	if (tstate == NULL)
		Py_FatalError("PyThreadState_Get: no current thread");
	return tstate;
}


/* An extension mechanism to store arbitrary additional per-thread state.
   PyThreadState_GetDict() returns a dictionary that can be used to hold such
   state; the caller should pick a unique key and store its state there.  If
   PyThreadState_GetDict() returns NULL, an exception has *not* been raised
   and the caller should assume no per-thread state is available. */

PyObject *
PyThreadState_GetDict(void)
{
	//PyThreadState *tstate = PyThread_get_key_value(autoTLSkey);
	PyThreadState *tstate = _py_local_tstate;
	if (tstate == NULL)
		return NULL;

	if (tstate->dict == NULL) {
		PyObject *d;
		tstate->dict = d = PyDict_New();
		if (d == NULL)
			PyErr_Clear();
	}
	return tstate->dict;
}


/* Asynchronously raise an exception in a thread.
   Requested by Just van Rossum and Alex Martelli.
   To prevent naive misuse, you must write your own extension
   to call this, or use ctypes.  Must be called with the GIL held.
   Returns the number of tstates modified (normally 1, but 0 if `id` didn't
   match any known thread id).  Can be called with exc=NULL to clear an
   existing async exception.  This raises no exceptions. */
/* XXX FIXME scrap and redesign */
int
PyThreadState_SetAsyncExc(long id, PyObject *exc) {
	PyThreadState *tstate = PyThreadState_Get();
	PyInterpreterState *interp = tstate->interp;
	PyThreadState *p;

	/* Although the GIL is held, a few C API functions can be called
	 * without the GIL held, and in particular some that create and
	 * destroy thread and interpreter states.  Those can mutate the
	 * list of thread states we're traversing, so to prevent that we lock
	 * head_mutex for the duration.
	 */
	HEAD_LOCK();
	for (p = interp->tstate_head; p != NULL; p = p->next) {
		if (p->thread_id == id) {
			/* Tricky:  we need to decref the current value
			 * (if any) in p->async_exc, but that can in turn
			 * allow arbitrary Python code to run, including
			 * perhaps calls to this function.  To prevent
			 * deadlock, we need to release head_mutex before
			 * the decref.
			 */
			PyObject *old_exc = p->async_exc;
			Py_XINCREF(exc);
			p->async_exc = exc;
			HEAD_UNLOCK();
			Py_XDECREF(old_exc);
			return 1;
		}
	}
	HEAD_UNLOCK();
	return 0;
}

/* Do periodic things.  This is called from the main event loop, so we
 * take care to reduce the per-call costs. */
int
PyThreadState_Tick(void)
{
	PyThreadState *tstate = PyThreadState_Get();

	if (AO_load_acquire(&tstate->inspect_flag)) {
#if 0
		PyState_Suspend();
		PyState_Resume();
#else
		PyThread_lock_release(tstate->inspect_lock);
		PyThread_lock_acquire(tstate->inspect_queue_lock);
		PyThread_lock_acquire(tstate->inspect_lock);
		PyThread_lock_release(tstate->inspect_queue_lock);
#endif
	}

	if (tstate->small_ticks > 0) {
		tstate->small_ticks--;
		return 0;
	} else {
		PyState_Suspend();
		PyState_Resume();

		if (Py_MakePendingCalls() < 0)
			return 1;

		if (tstate->async_exc != NULL) {
			PyObject *tmp = tstate->async_exc;
			tstate->async_exc = NULL;
			PyErr_SetNone(tmp);
			Py_DECREFTS(tmp);
			return 1;
		}

		if (PyErr_CheckSignals() < 0)
			return 1;

		tstate->large_ticks++;
		tstate->small_ticks = _Py_CheckInterval; /* XXX use atomic access? */

		return 0;
	}
}


/* Routines for advanced debuggers, requested by David Beazley.
   Don't use unless you know what you are doing! */
/* XXX FIXME not even slightly thread-safe!  These should be scrapped
   and redesigned! */
PyInterpreterState *
PyInterpreterState_Head(void)
{
	return interp_head;
}

PyInterpreterState *
PyInterpreterState_Next(PyInterpreterState *interp) {
	return interp->next;
}

PyThreadState *
PyInterpreterState_ThreadHead(PyInterpreterState *interp) {
	return interp->tstate_head;
}

PyThreadState *
PyThreadState_Next(PyThreadState *tstate) {
	return tstate->next;
}


/* The implementation of sys._current_frames().  This is intended to be
   called with the GIL held, as it will be when called via
   sys._current_frames().  It's possible it would work fine even without
   the GIL held, but haven't thought enough about that.
*/
PyObject *
_PyThread_CurrentFrames(void)
{
	PyObject *result;
	PyInterpreterState *i;

	result = PyDict_New();
	if (result == NULL)
		return NULL;

	/* for i in all interpreters:
	 *     for t in all of i's thread states:
	 *          if t's frame isn't NULL, map t's id to its frame
	 * Because these lists can mutute even when the GIL is held, we
	 * need to grab head_mutex for the duration.
	 */
	HEAD_LOCK();
	for (i = interp_head; i != NULL; i = i->next) {
		PyThreadState *t;
		for (t = i->tstate_head; t != NULL; t = t->next) {
			PyObject *id;
			int stat;
			struct _frame *frame = t->frame;
			if (frame == NULL)
				continue;
			id = PyInt_FromLong(t->thread_id);
			if (id == NULL)
				goto Fail;
			stat = PyDict_SetItem(result, id, (PyObject *)frame);
			Py_DECREF(id);
			if (stat < 0)
				goto Fail;
		}
	}
	HEAD_UNLOCK();
	return result;

 Fail:
 	HEAD_UNLOCK();
 	Py_DECREF(result);
 	return NULL;
}


static void
state_interrupt_callback(struct _PyInterruptQueue *queue, void *arg)
{
}

PyState_EnterFrame *
PyState_Enter(void)
{
	PyState_EnterFrame *enterframe;
	PyMonitorSpaceObject *monitorspace;
	PyMonitorSpaceFrame *monitorframe;

	enterframe = _PyState_EnterPreallocated(NULL);
	if (enterframe == NULL)
		return NULL;

#if 0
	monitorframe = malloc(sizeof(PyMonitorSpaceFrame));
	if (monitorframe == NULL) {
		_PyState_ExitSimple(enterframe);
		return NULL;
	}
	//*monitorframe = PyMonitorSpaceFrame_INIT;
	monitorframe->prevframe = NULL;
	monitorframe->monitorspace = NULL;

	monitorspace = (PyMonitorSpaceObject *)PyObject_CallObject((PyObject *)&PyMonitorSpace_Type, NULL);
	if (monitorspace == NULL) {
		free(monitorframe);
		_PyState_ExitSimple(enterframe);
		return NULL;
	}

	_PyMonitorSpace_Push(monitorframe, monitorspace);
#endif
	return enterframe;
}

PyState_EnterFrame *
_PyState_EnterPreallocated(PyThreadState *new_tstate)
{
	PyThreadState *tstate;
	PyState_EnterFrame *frame;
	static const PyMonitorSpaceFrame initframe = PyMonitorSpaceFrame_INIT;

	assert(autoInterpreterState);
	//tstate = (PyThreadState *)PyThread_get_key_value(autoTLSkey);
	tstate = _py_local_tstate;

	frame = malloc(sizeof(PyState_EnterFrame));
	if (frame == NULL)
		return NULL;

	if (tstate == NULL) {
		PyInterruptObject *point;
		/* Create a new thread state for this thread */
		if (new_tstate == NULL) {
			tstate = _PyThreadState_New();
			if (tstate == NULL) {
				free(frame);
				return NULL;
			}
		} else
			tstate = new_tstate;

		_PyThreadState_Bind(autoInterpreterState, tstate);
		frame->prevframe = tstate->enterframe;
		tstate->enterframe = frame;
		frame->locked = 0;
		frame->monitorspaceframe = initframe;

		PyState_Resume();
		point = PyInterrupt_New(state_interrupt_callback, NULL, NULL);
		if (point == NULL) {
			PyState_Suspend();
			_PyThreadState_Unbind(tstate);
			_PyThreadState_Delete(tstate);
			free(frame);
			return NULL;
		}
		PyInterrupt_Push(point);
		PyState_Suspend();
	} else {
		if (new_tstate != NULL)
			Py_FatalError("Unexpected new_tstate");

		if (tstate->enterframe->locked)
			PyState_Suspend();

		frame->prevframe = tstate->enterframe;
		tstate->enterframe = frame;
		frame->locked = 0;
		frame->monitorspaceframe = initframe;
	}

	PyState_Resume();
	return frame;
}

void
PyState_Exit(PyState_EnterFrame *enterframe)
{
#if 0
	PyThreadState *tstate = PyThreadState_Get();
	PyMonitorSpaceFrame *monitorframe = tstate->monitorspace_frame;
	PyMonitorSpaceObject *monitorspace = monitorframe->monitorspace;

	_PyMonitorSpace_Pop(monitorframe);
	Py_DECREF(monitorspace);
	free(monitorframe);
#endif
	_PyState_ExitSimple(enterframe);
}

void
_PyState_ExitSimple(PyState_EnterFrame *enterframe)
{
	PyThreadState *tstate = PyThreadState_Get();
	PyState_EnterFrame *oldframe;

	oldframe = tstate->enterframe;
	if (enterframe != oldframe)
		Py_FatalError("PyState_Exit called with wrong frame");

	if (tstate->suspended)
		Py_FatalError("PyState_Exit called while suspended");

	if (!oldframe->locked)
		Py_FatalError("PyState_Exit called in an unlocked state");

	if (oldframe->prevframe == NULL) {
		PyInterrupt_Pop(tstate->interrupt_point);
		Py_CLEAR(tstate->interrupt_point);
		PyThreadState_Clear(tstate);
		_PyThreadState_Unbind(tstate);
		_PyThreadState_Delete(tstate);
		free(oldframe);
	} else {
		PyState_Suspend();

		tstate->enterframe = oldframe->prevframe;
		free(oldframe);

		if (tstate->enterframe->locked)
			PyState_Resume();
	}
}


void
PyState_EnterImport(void)
{
	PyThreadState *tstate = PyThreadState_Get();

	if (AO_load_full(&tstate->interp->tstate_count) != 1)
		Py_FatalError("importing is not thread-safe");

	tstate->import_depth++;
}

void
PyState_ExitImport(void)
{
	PyThreadState *tstate = PyThreadState_Get();

	tstate->import_depth--;
	assert(tstate->import_depth >= 0);
}


/* Stops all other threads from accessing their PyState */
void
PyState_StopTheWorld(void)
{
	/* XXX FIXME */
}

void
PyState_StartTheWorld(void)
{
}


/* XXX change "active" MonitorSpace to none */
void
PyState_Suspend(void)
{
	int err = errno;
	PyThreadState *tstate = PyThreadState_Get();

	assert(!tstate->suspended);
	tstate->suspended = 1;
	tstate->enterframe->locked = 0;
	PyThread_lock_release(tstate->inspect_lock);

	errno = err;
}

/* XXX Reactivate MonitorSpace */
void
PyState_Resume(void)
{
	int err = errno;
	PyThreadState *tstate = PyThreadState_Get();

	assert(tstate->suspended);
	PyThread_lock_acquire(tstate->inspect_lock);
	tstate->suspended = 0;
	tstate->enterframe->locked = 1;

	errno = err;
}


PyCritical *
PyCritical_Allocate(Py_ssize_t depth)
{
	PyCritical *crit = malloc(sizeof(PyCritical));
	if (crit == NULL)
		return NULL;

	crit->lock = PyThread_lock_allocate();
	if (!crit->lock) {
		free(crit);
		return NULL;
	}

	crit->depth = depth;
	crit->prev = NULL;

	return crit;
}

void
PyCritical_Free(PyCritical *crit)
{
	PyThread_lock_free(crit->lock);
	free(crit);
}

void
PyCritical_Enter(PyCritical *crit)
{
	PyThreadState *tstate = PyThreadState_Get();

	assert(!tstate->suspended);

	if (tstate->critical_section != NULL &&
			tstate->critical_section->depth <= crit->depth)
		Py_FatalError("PyCritical_Enter called while "
			"already in deeper critical section");

	if (!_PyThread_lock_tryacquire(crit->lock)) {
		PyState_Suspend();
		PyThread_lock_acquire(crit->lock);
		PyState_Resume();
	}

	assert(crit->prev == NULL);
	crit->prev = tstate->critical_section;
	tstate->critical_section = crit;
}

void
PyCritical_Exit(PyCritical *crit)
{
	PyThreadState *tstate = PyThreadState_Get();

	assert(!tstate->suspended);

	if (tstate->critical_section != crit)
		Py_FatalError("PyCritical_Exit called with wrong "
			"critical section");

	tstate->critical_section = crit->prev;
	crit->prev = NULL;

	PyThread_lock_release(crit->lock);
}


extern PyThreadState * (*pymalloc_threadstate_hook)(void);

void
_PyState_InitThreads(void)
{
	if (interpreter_lock)
		Py_FatalError("Interpreter state already initialized");
	interpreter_lock = PyThread_lock_allocate();
	pending_readers = PyThread_cond_allocate();
	active_readers = PyThread_cond_allocate();
	main_thread = PyThread_get_thread_ident();
	autoTLSkey = PyThread_create_key();
	if (!interpreter_lock || !pending_readers || !active_readers ||
			!main_thread || !autoTLSkey)
		Py_FatalError("Allocation failed in _PyState_InitThreads");
	pymalloc_threadstate_hook = PyThreadState_Get;
}

void
PyState_PrepareFork(void)
{
	PyThread_lock_acquire(interpreter_lock);
}

void
PyState_CleanupForkParent(void)
{
	PyThread_lock_release(interpreter_lock);
}

/* This function is called from PyOS_AfterFork to reset the interpreter's
   locks so they can be used in the child process.  (This could also be
   done using pthread_atfork mechanism, at least for the pthreads
   implementation.)

   However, this only resets a few of the locks that may be in use, so
   doing anything non-trivial is almost certainly unsafe.

   Cleaning up properly is impossible.  The users of fork should really
   be rewritten in C so they don't need to touch python objects before
   they exec.  For now, we bodge things.*/
void
PyState_CleanupForkChild(void)
{
	PyState_EnterTag entertag;

	if (!interpreter_lock)
		return;

	interpreter_lock = PyThread_lock_allocate();
	if (!interpreter_lock)
		Py_FatalError("Unable to allocate lock");
	pending_readers_count = 0;
	active_readers_count = 0;
	active_writer = 0;
	pending_writers = NULL;
	PyThread_delete_key_value(autoTLSkey);
	entertag = PyState_Enter();
	if (!entertag)
		Py_FatalError("Unable to re-enter state after fork");
	main_thread = PyThread_get_thread_ident();
}

/* Internal initialization/finalization functions called by
   Py_Initialize/Py_Finalize
*/
void
_PyState_Fini(void)
{
	PyThread_delete_key(autoTLSkey);
	autoTLSkey = 0;
}

int
PyState_CurrentIsMain(void)
{
	return !main_thread || PyThread_get_thread_ident() == main_thread;
}


#ifdef __cplusplus
}
#endif
