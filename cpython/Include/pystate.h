
/* Thread and interpreter state structures and their interfaces */


#ifndef Py_PYSTATE_H
#define Py_PYSTATE_H
#ifdef __cplusplus
extern "C" {
#endif

#include <atomic_ops.h>

#include "pythread.h"


/* XXX Must match up with gcmodule.c's gc_cache_size_classes */
#define PYGC_CACHE_SIZECLASSES 13

#define PYGC_CACHE_COUNT 32

/* XXX Must be a power of 2 */
//#define Py_ASYNCREFCOUNT_TABLE 1024
#define Py_ASYNCREFCOUNT_TABLE 2048


/* State shared between threads */

struct _ts; /* Forward */
struct _is; /* Forward */
struct _PyMonitorSpaceFrame; /* Forward */
struct _PyState_EnterFrame;
typedef struct _PyState_EnterFrame *PyState_EnterTag;

typedef struct _is {

    struct _is *next;
    struct _ts *tstate_head;
    AO_t tstate_count;
    PyState_EnterTag entertag;

    PyObject *modules;
    PyObject *sysdict;
    PyObject *builtins;
    PyObject *modules_reloading;

    PyObject *codec_search_path;
    PyObject *codec_search_cache;
    PyObject *codec_error_registry;

#ifdef HAVE_DLOPEN
    int dlopenflags;
#endif
#ifdef WITH_TSC
    int tscdump;
#endif

} PyInterpreterState;


/* State unique per thread */

struct _frame; /* Avoid including frameobject.h */
struct _PyMonitorSpaceObject; /* Avoid including monitorobject.h */
struct _PyInterruptObject; /* Avoid including interruptobject.h */

/* Py_tracefunc return -1 when raising an exception, or 0 for success. */
typedef int (*Py_tracefunc)(PyObject *, struct _frame *, int, PyObject *);

/* The following values are used for 'what' for tracefunc functions: */
#define PyTrace_CALL 0
#define PyTrace_EXCEPTION 1
#define PyTrace_LINE 2
#define PyTrace_RETURN 3
#define PyTrace_C_CALL 4
#define PyTrace_C_EXCEPTION 5
#define PyTrace_C_RETURN 6

typedef struct _PyMonitorSpaceFrame {
	struct _PyMonitorSpaceFrame *prevframe;
	struct _PyMonitorSpaceObject *monitorspace;
} PyMonitorSpaceFrame;

#define PyMonitorSpaceFrame_INIT {NULL, NULL}

typedef struct _PyState_EnterFrame {
	struct _PyState_EnterFrame *prevframe;
	PyMonitorSpaceFrame monitorspaceframe;
	int locked;
} PyState_EnterFrame;

typedef struct _PyCritical {
    PyThread_type_lock lock;
    Py_ssize_t depth;
    struct _PyCritical *prev;
} PyCritical;

struct _object;  /* From object.h, which includes us.  Doh! */

typedef struct {
	struct _object *obj;
	AO_t diff;
} PyAsyncRefEntry;

typedef struct _ts {
    /* See Python/ceval.c for comments explaining most fields */

    struct _ts *next;
    PyInterpreterState *interp;

    struct _frame *frame;
    int recursion_depth;
    char overflowed; /* The stack has overflowed. Allow 50 more calls
                        to handle the runtime error. */
    char recursion_critical; /* The current calls must not cause
                                a stack overflow. */
    int dealloc_depth;
    /* 'tracing' keeps track of the execution depth when tracing/profiling.
       This is to prevent the actual trace/profile code from being recorded in
       the trace/profile. */
    int tracing;
    int use_tracing;

    Py_tracefunc c_profilefunc;
    Py_tracefunc c_tracefunc;
    PyObject *c_profileobj;
    PyObject *c_traceobj;

    PyObject *curexc_type;
    PyObject *curexc_value;
    PyObject *curexc_traceback;

    PyObject *exc_type;
    PyObject *exc_value;
    PyObject *exc_traceback;

    PyObject *dict;  /* Stores per-thread state */

    /* large_ticks is incremented whenever the check_interval ticker
     * reaches zero. The purpose is to give a useful measure of the number
     * of interpreted bytecode instructions in a given thread.  This
     * extremely lightweight statistic collector may be of interest to
     * profilers (like psyco.jit()), although nothing in the core uses it.
     */
    int large_ticks;
    int small_ticks;

    AO_t inspect_count;
    PyThread_type_lock inspect_queue_lock;
    PyThread_type_lock inspect_lock;
    AO_t inspect_flag;

    int suspended;

    PyState_EnterFrame *enterframe;

    Py_ssize_t import_depth;
    PyMonitorSpaceFrame *monitorspace_frame;
    PyMonitorSpaceFrame _base_monitorspace_frame;

    struct _PyInterruptObject *interrupt_point;

    /* Simple lock that doesn't employ deadlock detection */
    PyCritical *critical_section;

    /* The Monitor Space lock that this thread may be blocked on. */
    struct _PyMonitorSpaceObject *active_lock;
    struct _ts *lockwait_prev;
    struct _ts *lockwait_next;
    PyThread_type_cond lockwait_cond;

    /* XXX signal handlers should also be here */

    void *malloc_cache[PYMALLOC_CACHE_SIZECLASSES][PYMALLOC_CACHE_COUNT];
    void *gc_object_cache[PYGC_CACHE_SIZECLASSES][PYGC_CACHE_COUNT];

    PyAsyncRefEntry async_refcounts[Py_ASYNCREFCOUNT_TABLE];
} PyThreadState;


PyAPI_FUNC(PyInterpreterState *) PyInterpreterState_New(void);
PyAPI_FUNC(void) PyInterpreterState_Clear(PyInterpreterState *);
PyAPI_FUNC(void) PyInterpreterState_Delete(PyInterpreterState *);

PyAPI_FUNC(PyThreadState *) _PyThreadState_New(void);
PyAPI_FUNC(void) PyThreadState_Clear(PyThreadState *);
PyAPI_FUNC(void) _PyThreadState_Delete(PyThreadState *tstate);

PyAPI_FUNC(PyThreadState *) _PyThreadState_Get(void);
#ifdef Py_BUILD_CORE
PyAPI_DATA(__thread PyThreadState *) _py_local_tstate;
static inline PyThreadState *
PyThreadState_Get(void)
{
	PyThreadState *tstate = _py_local_tstate;
	if (tstate == NULL)
		Py_FatalError("PyThreadState_Get: no current thread");
	return tstate;
}
#else
#define PyThreadState_Get _PyThreadState_Get
#endif
PyAPI_FUNC(PyObject *) PyThreadState_GetDict(void);
PyAPI_FUNC(int) PyThreadState_SetAsyncExc(long, PyObject *);


/* Ensure that the current thread is ready to call the Python
   C API, regardless of the current state of Python, or of its
   thread lock.  This may be called as many times as desired
   by a thread so long as each call is matched with a call to
   PyGILState_Release().  In general, other thread-state APIs may
   be used between _Ensure() and _Release() calls, so long as the
   thread-state is restored to its previous state before the Release().
   For example, normal use of the Py_BEGIN_ALLOW_THREADS/
   Py_END_ALLOW_THREADS macros are acceptable.

   The return value is an opaque "handle" to the thread state when
   PyGILState_Ensure() was called, and must be passed to
   PyGILState_Release() to ensure Python is left in the same state. Even
   though recursive calls are allowed, these handles can *not* be shared -
   each unique call to PyGILState_Ensure must save the handle for its
   call to PyGILState_Release.

   When the function returns, the current thread will hold the GIL.

   0 is returned if memory is unavailable.
*/
PyAPI_FUNC(PyState_EnterTag) PyState_Enter(void);
PyAPI_FUNC(PyState_EnterTag) _PyState_EnterPreallocated(PyThreadState *);

/* Release any resources previously acquired.  After this call, Python's
   state will be the same as it was prior to the corresponding
   PyGILState_Ensure() call (but generally this state will be unknown to
   the caller, hence the use of the GILState API.)

   Every call to PyGILState_Ensure must be matched by a call to
   PyGILState_Release on the same thread.
*/
PyAPI_FUNC(void) PyState_Exit(PyState_EnterTag);
PyAPI_FUNC(void) _PyState_ExitSimple(PyState_EnterFrame *);

/* The implementation of sys._current_frames()  Returns a dict mapping
   thread id to that thread's current frame.
*/
PyAPI_FUNC(PyObject *) _PyThread_CurrentFrames(void);

/* Routines for advanced debuggers, requested by David Beazley.
   Don't use unless you know what you are doing! */
PyAPI_FUNC(PyInterpreterState *) PyInterpreterState_Head(void);
PyAPI_FUNC(PyInterpreterState *) PyInterpreterState_Next(PyInterpreterState *);
PyAPI_FUNC(PyThreadState *) PyInterpreterState_ThreadHead(PyInterpreterState *);
PyAPI_FUNC(PyThreadState *) PyThreadState_Next(PyThreadState *);

typedef struct _frame *(*PyThreadFrameGetter)(PyThreadState *self_);

/* hook for PyEval_GetFrame(), requested for Psyco */
PyAPI_DATA(PyThreadFrameGetter) _PyThreadState_GetFrame;

PyAPI_FUNC(int) PyThreadState_Tick(void);


PyAPI_FUNC(void) PyState_EnterImport(void);
PyAPI_FUNC(void) PyState_ExitImport(void);

PyAPI_FUNC(void) PyState_StopTheWorld(void);
PyAPI_FUNC(void) PyState_StartTheWorld(void);


/* Prefered API for locking if PyState is involved.  Required if
 * Py_INCREF/Py_DECREF are used.  The code is assumed to be a critical
 * section (involving a known, fixed amount of code; entering other
 * critical sections is an error.)  PyState_Suspend might be called
 * while entering. */
PyAPI_FUNC(PyCritical *) PyCritical_Allocate(Py_ssize_t);
PyAPI_FUNC(void) PyCritical_Free(PyCritical *);
PyAPI_FUNC(void) PyCritical_Enter(PyCritical *);
PyAPI_FUNC(void) PyCritical_Exit(PyCritical *);
/* This is just a bodge for deathqueue_wait.  It shouldn't be used in general */
PyAPI_FUNC(void) _PyCritical_CondWait(PyCritical *, PyThread_type_cond);

/* Most code only needs one critical section at a time.  They should use
 * PyCRITICAL_NORMAL and be done with it.  Occasionally you'll need two
 * specific critical sections at once, in which case you should add your
 * defines and document them with your own little graph here.
 *
 * If there's enough independent graphs I may also add a "section" field,
 * so you can't accidentally mix graphs.  Will I ever have a need for
 * "universal" critical sections though, that can be entered while in
 * any other critical section (except other universal critical sections)?
 * XXX Using a stack-allocated critical section for INCREF/DECREF and the
 * like would need a "universal" section field.
 *
 *    WEAKREF
 *       |
 *    HANDLE
 *       |
 *     QUEUE
 */
#define PyCRITICAL_WEAKREF_REF 2
#define PyCRITICAL_WEAKREF_HANDLE 1
#define PyCRITICAL_WEAKREF_QUEUE 0
#define PyCRITICAL_NORMAL 0


/* Interface for threads.

   A module that plans to do a blocking system call (or something else
   that lasts a long time and doesn't touch Python data) can allow other
   threads to run as follows:

	...preparations here...
	Py_BEGIN_ALLOW_THREADS
	...blocking system call here...
	Py_END_ALLOW_THREADS
	...interpret result here...

   The Py_BEGIN_ALLOW_THREADS/Py_END_ALLOW_THREADS pair expands to a
   {}-surrounded block.
   To leave the block in the middle (e.g., with return), you must insert
   a line containing Py_BLOCK_THREADS before the return, e.g.

	if (...premature_exit...) {
		Py_BLOCK_THREADS
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}

   An alternative is:

	Py_BLOCK_THREADS
	if (...premature_exit...) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}
	Py_UNBLOCK_THREADS

   For convenience, that the value of 'errno' is restored across
   Py_END_ALLOW_THREADS and Py_BLOCK_THREADS.

   WARNING: NEVER NEST CALLS TO Py_BEGIN_ALLOW_THREADS AND
   Py_END_ALLOW_THREADS!!!

   The function PyEval_InitThreads() should be called only from
   initthread() in "threadmodule.c".

   Note that not yet all candidates have been converted to use this
   mechanism!
*/

#ifndef WITH_THREAD
#error Threading support is now unconditional
#endif

PyAPI_FUNC(void) PyState_PrepareFork(void);
PyAPI_FUNC(void) PyState_CleanupForkParent(void);
PyAPI_FUNC(void) PyState_CleanupForkChild(void);
PyAPI_FUNC(void) PyState_Suspend(void);
PyAPI_FUNC(void) PyState_Resume(void);

#define Py_BEGIN_ALLOW_THREADS PyState_Suspend();
#define Py_BLOCK_THREADS PyState_Resume();
#define Py_UNBLOCK_THREADS PyState_Suspend();
#define Py_END_ALLOW_THREADS PyState_Resume();

PyAPI_FUNC(int) _PyEval_SliceIndex(PyObject *, Py_ssize_t *);


#ifdef __cplusplus
}
#endif
#endif /* !Py_PYSTATE_H */
