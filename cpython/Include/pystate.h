
/* Thread and interpreter state structures and their interfaces */


#ifndef Py_PYSTATE_H
#define Py_PYSTATE_H
#ifdef __cplusplus
extern "C" {
#endif

#include <atomic_ops.h>

#include "pythread.h"
#include "pylinkedlist.h"


/* XXX Must match up with gcmodule.c's gc_cache_size_classes */
#define PYGC_CACHE_SIZECLASSES 13

#define PYGC_CACHE_COUNT 32

/* XXX Must be a power of 2 */
//#define Py_ASYNCREFCOUNT_TABLE 1024
#define Py_ASYNCREFCOUNT_TABLE 2048


/* State shared between threads */

struct _PyState; /* Forward */
struct _PyMonitorSpaceFrame; /* Forward */
struct _PyState_EnterFrame; /* Forward */

struct _frame; /* Avoid including frameobject.h */
struct _PyMonitorSpaceObject; /* Avoid including monitorobject.h */
struct _PyCancelObject; /* Avoid including cancelobject.h */

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

struct _PyWaitFor;
struct _PyWaitFor_Inspection;
typedef void (*py_abortfunc)(struct _PyWaitFor_Inspection *,
    struct _PyWaitFor *node);

typedef struct _PyWaitFor {
    void *self;
    PyThread_type_lock *lock;
    struct _PyWaitFor *blocker;
    int checking_deadlock;
    PyLinkedListNode inspection_links;
    py_abortfunc abortfunc;
} PyWaitFor;

typedef struct _PyWaitFor_Inspection {
    PyLinkedList inspecting;
    int global;
} PyWaitFor_Inspection;

typedef struct _PyMonitorSpaceFrame {
    struct _PyMonitorSpaceObject *monitorspace;
    PyLinkedListNode links;
} PyMonitorSpaceFrame;

typedef struct _PyState_EnterFrame {
    struct _PyState_EnterFrame *prevframe;
    PyMonitorSpaceFrame monitorspaceframe;
    int locked;
} PyState_EnterFrame;

typedef struct _PyCritical {
    PyThread_type_lock *lock;
    Py_ssize_t depth;
    struct _PyCritical *prev;
} PyCritical;

struct _object;  /* From object.h, which includes us.  Doh! */

typedef struct {
    struct _object *obj;
    AO_t diff;
} PyAsyncRefEntry;

typedef struct _PyState {
    /* See Python/ceval.c for comments explaining most fields */

    struct _PyState *next;
    int used;
    int deleted;

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

    PyThread_type_lock *thread_lock;
    PyThread_type_sem *world_wakeup;
    PyLinkedListNode world_wakeup_links;

    PyThread_type_lock *refowner_lock;
    PyThread_type_lock *refowner_waiting_lock;
    AO_t refowner_waiting_flag;

    int suspended;

    PyState_EnterFrame *enterframe;

    Py_ssize_t import_depth;
    PyLinkedList monitorspaces;

    PyLinkedList cancel_stack;
    PyCritical *cancel_crit;

    /* Simple lock that doesn't employ deadlock detection */
    PyCritical *critical_section;

    /* The Monitor Space lock that this thread may be blocked on */
    //struct _PyMonitorSpaceObject *active_lock;
    //struct _PyState *lockwait_prev;
    //struct _PyState *lockwait_next;
    //PyThread_type_cond *lockwait_cond;
    PyWaitFor waitfor;
    PyThread_type_timeout *monitorspace_timeout;
    //PyMonitorSpaceObject *monitorspace_wanted;
    PyLinkedListNode monitorspace_waitinglinks;
    PyThread_type_flag *monitorspace_waitingflag;

    /* The Monitor condition that this thread may be waiting on */
    PyLinkedListNode condition_links;
    PyThread_type_flag *condition_flag;

    /* XXX signal handlers should also be here */

    void *malloc_cache[PYMALLOC_CACHE_SIZECLASSES][PYMALLOC_CACHE_COUNT];
    void *gc_object_cache[PYGC_CACHE_SIZECLASSES][PYGC_CACHE_COUNT];

    PyAsyncRefEntry async_refcounts[Py_ASYNCREFCOUNT_TABLE];
} PyState;


PyAPI_FUNC(int) _PyState_SingleThreaded(void);

PyAPI_FUNC(PyState *) _PyState_New(void);
PyAPI_FUNC(void) _PyState_Delete(PyState *);

PyAPI_FUNC(PyState *) _PyState_Get(void);
#if defined(Py_BUILD_CORE) && defined(HAVE_THREAD_LOCAL_VARIABLE)
PyAPI_DATA(__thread PyState *) _py_local_pystate;
static inline PyState *
PyState_Get(void)
{
    PyState *pystate = _py_local_pystate;
    if (pystate == NULL)
        Py_FatalError("PyState_Get: no current thread");
    return pystate;
}
#else
#define PyState_Get _PyState_Get
#endif

PyAPI_FUNC(PyObject *) PyState_GetDict(void);


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
PyAPI_FUNC(PyState_EnterFrame *) PyState_Enter(void);
PyAPI_FUNC(int) _PyState_EnterPreallocated(PyState_EnterFrame *, PyState *);

/* Release any resources previously acquired.  After this call, Python's
   state will be the same as it was prior to the corresponding
   PyGILState_Ensure() call (but generally this state will be unknown to
   the caller, hence the use of the GILState API.)

   Every call to PyGILState_Ensure must be matched by a call to
   PyGILState_Release on the same thread.
*/
PyAPI_FUNC(void) PyState_Exit(PyState_EnterFrame *);
PyAPI_FUNC(void) _PyState_ExitPreallocated(PyState_EnterFrame *);

typedef struct _frame *(*PyThreadFrameGetter)(PyState *self_);

/* hook for PyEval_GetFrame(), requested for Psyco */
PyAPI_DATA(PyThreadFrameGetter) _PyState_GetFrame;

PyAPI_FUNC(int) PyState_Tick(void);


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
PyAPI_FUNC(void) _PyCritical_CondWait(PyCritical *, PyThread_type_cond *);

/* Dummy critical sections, allocated on a stack and only used by one
 * thread, used only to limit the effects of PyState_Suspend (and
 * thereby preventing PyState_StopTheWorld from functioning). */
PyAPI_FUNC(void) PyCritical_EnterDummy(PyCritical *, Py_ssize_t);
PyAPI_FUNC(void) PyCritical_ExitDummy(PyCritical *);

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
#define PyCRITICAL_DEALLOC 100

#define PyCRITICAL_WEAKREF_REF 2
#define PyCRITICAL_WEAKREF_HANDLE 1
#define PyCRITICAL_WEAKREF_QUEUE 0

#define PyCRITICAL_NORMAL 0

#define PyCRITICAL_CANCEL -1

#define PyCRITICAL_REFMODE_PROMOTE -100


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

PyAPI_FUNC(void) PyState_Suspend(void);
PyAPI_FUNC(void) PyState_Resume(void);
PyAPI_FUNC(void) PyState_MaybeSuspend(void);
PyAPI_FUNC(void) PyState_MaybeResume(void);

#define Py_BEGIN_ALLOW_THREADS PyState_Suspend();
#define Py_BLOCK_THREADS PyState_Resume();
#define Py_UNBLOCK_THREADS PyState_Suspend();
#define Py_END_ALLOW_THREADS PyState_Resume();

PyAPI_FUNC(int) _PyEval_SliceIndex(PyObject *, Py_ssize_t *);

PyAPI_FUNC(void) _PyState_FlushAsyncRefcounts(void);


#ifdef __cplusplus
}
#endif
#endif /* !Py_PYSTATE_H */
