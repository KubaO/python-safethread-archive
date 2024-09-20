
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


#include "pythread.h"

#ifdef __cplusplus
extern "C" {
#endif

static PyThread_type_key autoTLSkey = 0;
static AO_t thread_count;

static PyState *pystate_head;

static PyThread_type_lock world_lock;
static PyThread_type_lock world_wakeup_lock;
static PyLinkedList world_wakeup_list = {&world_wakeup_list, &world_wakeup_list};
static AO_t world_sleep;

/* This hook exists so psyco can provide it's own frame objects */
static struct _frame *threadstate_getframe(PyState *self);
PyThreadFrameGetter _PyState_GetFrame = threadstate_getframe;

__thread PyState *_py_local_pystate;


int
_PyState_SingleThreaded(void)
{
    return AO_load_full(&thread_count) == 1;
}


/* Default implementation for _PyState_GetFrame */
static struct _frame *
threadstate_getframe(PyState *self)
{
    return self->frame;
}

PyState *
_PyState_New(void)
{
    PyState *pystate;
    int i, j;

    pystate = malloc(sizeof(PyState));
    if (pystate == NULL)
        return NULL;

    pystate->used = 0;
    pystate->deleted = 0;

    pystate->frame = NULL;
    pystate->recursion_depth = 0;
    pystate->overflowed = 0;
    pystate->recursion_critical = 0;
    pystate->dealloc_depth = 0;
    pystate->tracing = 0;
    pystate->use_tracing = 0;

    pystate->large_ticks = 0;
    pystate->small_ticks = 0;

    pystate->thread_lock = NULL;
    pystate->world_wakeup = NULL;
    pystate->world_wakeup_links.prev = NULL;
    pystate->world_wakeup_links.next = NULL;

    pystate->refowner_lock = NULL;
    pystate->refowner_waiting_lock = NULL;
    pystate->refowner_waiting_flag = 0;

    pystate->suspended = 1;

    pystate->enterframe = NULL;

    pystate->dict = NULL;

    pystate->curexc_type = NULL;
    pystate->curexc_value = NULL;
    pystate->curexc_traceback = NULL;

    pystate->exc_type = NULL;
    pystate->exc_value = NULL;
    pystate->exc_traceback = NULL;

    pystate->c_profilefunc = NULL;
    pystate->c_tracefunc = NULL;
    pystate->c_profileobj = NULL;
    pystate->c_traceobj = NULL;

    pystate->import_depth = 0;
    pystate->monitorspace_frame = &pystate->_base_monitorspace_frame;
    pystate->_base_monitorspace_frame.prevframe = NULL;
    pystate->_base_monitorspace_frame.monitorspace = NULL;

    for (i = 0; i < PYMALLOC_CACHE_SIZECLASSES; i++) {
        for (j = 0; j < PYMALLOC_CACHE_COUNT; j++)
            pystate->malloc_cache[i][j] = NULL;
    }

    for (i = 0; i < PYGC_CACHE_SIZECLASSES; i++) {
        for (j = 0; j < PYGC_CACHE_COUNT; j++)
            pystate->gc_object_cache[i][j] = NULL;
    }

    for (i = 0; i < Py_ASYNCREFCOUNT_TABLE; i ++) {
        pystate->async_refcounts[i].obj = NULL;
        pystate->async_refcounts[i].diff = 0;
    }

    pystate->interrupt_point = NULL;

    pystate->critical_section = NULL;

    pystate->active_lock = NULL;
    pystate->lockwait_prev = NULL;
    pystate->lockwait_next = NULL;
    pystate->lockwait_cond = NULL;

    pystate->lockwait_cond = PyThread_cond_allocate();
    pystate->thread_lock = PyThread_lock_allocate();
    pystate->world_wakeup = PyThread_sem_allocate(0);

    pystate->refowner_lock = PyThread_lock_allocate();
    pystate->refowner_waiting_lock = PyThread_lock_allocate();

    if (!pystate->lockwait_cond || !pystate->thread_lock ||
            !pystate->world_wakeup || !pystate->refowner_lock ||
            !pystate->refowner_waiting_lock)
        goto failed;

    //printf("New pystate %p\n", pystate);
    return pystate;

failed:
    if (pystate->lockwait_cond)
        PyThread_cond_free(pystate->lockwait_cond);
    if (pystate->thread_lock)
        PyThread_lock_free(pystate->thread_lock);
    if (pystate->world_wakeup)
        PyThread_sem_free(pystate->world_wakeup);

    if (pystate->refowner_lock)
        PyThread_lock_free(pystate->refowner_lock);
    if (pystate->refowner_waiting_lock)
        PyThread_lock_free(pystate->refowner_waiting_lock);
    free(pystate);
    return NULL;
}

void
_PyState_Delete(PyState *pystate)
{
    if (pystate->used)
        Py_FatalError("Cannot delete used PyState");

    assert(pystate->critical_section == NULL);
    assert(pystate->suspended);
    assert(pystate->monitorspace_frame == &pystate->_base_monitorspace_frame);
    assert(pystate->monitorspace_frame->monitorspace == NULL);
    assert(pystate->enterframe == NULL);
    assert(!pystate->deleted);

    /* pystate was never bound, or the tracing GC has cleaned it up */
    /* XXX FIXME nothing currently does this, and they're never
     * removed from the linked list */
    //fprintf(stderr, "Deleting pystate %p\n", pystate);
    PyThread_cond_free(pystate->lockwait_cond);
    PyThread_lock_free(pystate->thread_lock);
    PyThread_sem_free(pystate->world_wakeup);

    PyThread_lock_free(pystate->refowner_lock);
    PyThread_lock_free(pystate->refowner_waiting_lock);
    free(pystate);
}

static void
_PyState_Bind(PyState *pystate)
{
    assert(autoTLSkey);
    if (PyThread_get_key_value(autoTLSkey) != 0)
        Py_FatalError("Thread already has PyState");

    assert(!pystate->used);
    assert(pystate->deleted == 0);
    pystate->used = 1;
    PyThread_set_key_value(autoTLSkey, pystate);
    _py_local_pystate = pystate;

    AO_fetch_and_add1_full(&thread_count);

    PyThread_lock_acquire(world_lock);
    pystate->next = pystate_head;
    pystate_head = pystate;
    PyThread_lock_release(world_lock);
}

/* Undoes the work of _New, _Bind, and _Resume */
static void
_PyState_Terminate(PyState *pystate)
{
    assert(pystate != NULL && pystate == PyState_Get());
    assert(!pystate->suspended);
    assert(pystate->monitorspace_frame == &pystate->_base_monitorspace_frame);
    assert(pystate->monitorspace_frame->monitorspace == NULL);
    assert(pystate->enterframe == NULL);
    assert(pystate->used);
    assert(!pystate->deleted);

    _PyGC_Object_Cache_Flush();
    _PyGC_AsyncRefcount_Flush(pystate);

    /* Undo _Bind */
    AO_fetch_and_sub1_full(&thread_count);
    PyThread_delete_key_value(autoTLSkey);
    _py_local_pystate = NULL;

    /* Indirectly undo _New.  The tracing GC will do the real work. */
    pystate->deleted = 1;

    /* Undo _Resume */
    pystate->suspended = 1;
    PyThread_lock_release(pystate->refowner_lock);
    PyThread_lock_release(pystate->thread_lock);
}

static void
_PyState_Clear(PyState *pystate)
{
    Py_CLEAR(pystate->frame);

    Py_CLEAR(pystate->dict);

    Py_CLEAR(pystate->curexc_type);
    Py_CLEAR(pystate->curexc_value);
    Py_CLEAR(pystate->curexc_traceback);

    Py_CLEAR(pystate->exc_type);
    Py_CLEAR(pystate->exc_value);
    Py_CLEAR(pystate->exc_traceback);

    pystate->c_profilefunc = NULL;
    pystate->c_tracefunc = NULL;
    Py_CLEAR(pystate->c_profileobj);
    Py_CLEAR(pystate->c_traceobj);
}


PyState *
_PyState_Get(void)
{
    //PyState *pystate = PyThread_get_key_value(autoTLSkey);
    PyState *pystate = _py_local_pystate;
    if (pystate == NULL)
        Py_FatalError("PyState_Get: no current thread");
    return pystate;
}


/* An extension mechanism to store arbitrary additional per-thread state.
   PyState_GetDict() returns a dictionary that can be used to hold such
   state; the caller should pick a unique key and store its state there.  If
   PyState_GetDict() returns NULL, an exception has *not* been raised
   and the caller should assume no per-thread state is available. */

PyObject *
PyState_GetDict(void)
{
    //PyState *pystate = PyThread_get_key_value(autoTLSkey);
    PyState *pystate = _py_local_pystate;
    if (pystate == NULL)
        return NULL;

    if (pystate->dict == NULL) {
        PyObject *d;
        pystate->dict = d = PyDict_New();
        if (d == NULL)
            PyErr_Clear();
    }
    return pystate->dict;
}


PyState_EnterFrame *
PyState_Enter(void)
{
    PyState_EnterFrame *frame;

    frame = malloc(sizeof(PyState_EnterFrame));
    if (frame == NULL)
        return NULL;

    if (_PyState_EnterPreallocated(frame, NULL)) {
        free(frame);
        return NULL;
    }

    return frame;
}

int
_PyState_EnterPreallocated(PyState_EnterFrame *frame, PyState *pystate)
{
    PyState *old_pystate;

    //pystate = (PyState *)PyThread_get_key_value(autoTLSkey);
    old_pystate = _py_local_pystate;

    if (old_pystate == NULL) {
        /* Create a new thread state for this thread */
        if (pystate == NULL) {
            pystate = _PyState_New();
            if (pystate == NULL)
                return 1;
        }

        _PyState_Bind(pystate);
    } else {
        if (pystate != NULL)
            Py_FatalError("Unexpected new_pystate");
        pystate = old_pystate;

        if (pystate->enterframe->locked)
            PyState_Suspend();
    }

    frame->prevframe = pystate->enterframe;
    pystate->enterframe = frame;
    frame->locked = 0;
    frame->monitorspaceframe.prevframe = pystate->monitorspace_frame;
    frame->monitorspaceframe.monitorspace = NULL;
    pystate->monitorspace_frame = &frame->monitorspaceframe;

    PyState_Resume();
    return 0;
}

void
PyState_Exit(PyState_EnterFrame *frame)
{
    _PyState_ExitPreallocated(frame);
    free(frame);
}

/* This consumes the pystate originally passed in to
 * _PyState_EnterPreallocated, if any */
void
_PyState_ExitPreallocated(PyState_EnterFrame *frame)
{
    PyState *pystate = PyState_Get();

    if (frame != pystate->enterframe)
        Py_FatalError("PyState_Exit called with wrong frame");
    if (pystate->suspended)
        Py_FatalError("PyState_Exit called while suspended");
    if (!frame->locked)
        Py_FatalError("PyState_Exit called in an unlocked state");

    if (frame->prevframe == NULL) {
        assert(pystate->interrupt_point == NULL);

        assert(pystate->monitorspace_frame == &frame->monitorspaceframe);
        Py_CLEAR(pystate->monitorspace_frame->monitorspace);
        _PyState_Clear(pystate);

        assert(pystate->monitorspace_frame->monitorspace == NULL);
        pystate->monitorspace_frame = pystate->monitorspace_frame->prevframe;
        pystate->enterframe = frame->prevframe;

        _PyState_Terminate(pystate);
    } else {
        PyState_Suspend();

        assert(pystate->monitorspace_frame == &frame->monitorspaceframe);
        pystate->monitorspace_frame = pystate->monitorspace_frame->prevframe;
        Py_XDECREF(frame->monitorspaceframe.monitorspace);

        pystate->enterframe = frame->prevframe;

        if (pystate->enterframe->locked)
            PyState_Resume();
    }
}


void
PyState_EnterImport(void)
{
    PyState *pystate = PyState_Get();

    pystate->import_depth++;
}

void
PyState_ExitImport(void)
{
    PyState *pystate = PyState_Get();

    pystate->import_depth--;
    assert(pystate->import_depth >= 0);
}


/* Stops all other threads from accessing their PyState */
void
PyState_StopTheWorld(void)
{
    PyState *t;
    PyState *pystate = PyState_Get();

    //fprintf(stderr, "%p Stopping the world\n", pystate);
    assert(!pystate->suspended);
    if (pystate->critical_section != NULL)
        Py_FatalError("PyState_StopTheWorld cannot be called while in "
            "a critical section");

    PyState_Suspend();
    PyThread_lock_acquire(world_lock);
    AO_store_full(&world_sleep, 1);

    t = pystate_head;
    while (t != NULL) {
        if (t != pystate)
            PyThread_lock_acquire(t->thread_lock);
        t = t->next;
    }

    PyState_Resume();
}

void
PyState_StartTheWorld(void)
{
    PyState *t;
    PyState *pystate = PyState_Get();

    //fprintf(stderr, "%p Starting the world\n", pystate);
    AO_store_full(&world_sleep, 0);

    t = pystate_head;
    while (t != NULL) {
        if (t != pystate)
            PyThread_lock_release(t->thread_lock);
        t = t->next;
    }

    PyThread_lock_acquire(world_wakeup_lock);

    while (!PyLinkedList_Empty(&world_wakeup_list)) {
        t = PyLinkedList_Restore(PyState, world_wakeup_links, world_wakeup_list.next);
        assert(t != pystate);
        PyLinkedList_Remove(&t->world_wakeup_links);
        PyThread_sem_release(t->world_wakeup);
    }

    PyThread_lock_release(world_wakeup_lock);

    PyThread_lock_release(world_lock);
}


void
PyState_Suspend(void)
{
    PyState *pystate = PyState_Get();
    if (pystate->critical_section != NULL)
        Py_FatalError("PyState_Suspend called while in a critical section");
    PyState_MaybeSuspend();
}

void
PyState_Resume(void)
{
    PyState *pystate = PyState_Get();
    if (pystate->critical_section != NULL)
        Py_FatalError("PyState_Resume called while in a critical section");
    PyState_MaybeResume();
}

void
PyState_MaybeSuspend(void)
{
    int err = errno;
    PyState *pystate = PyState_Get();

    //fprintf(stderr, "%p Suspending\n", pystate);
    assert(!pystate->suspended);
    pystate->suspended = 1;
    pystate->enterframe->locked = 0;
    PyThread_lock_release(pystate->refowner_lock);
    /* XXX FIXME add a SuspendRefowner that doesn't release thread_lock? */
    if (pystate->critical_section == NULL)
        PyThread_lock_release(pystate->thread_lock);
    //fprintf(stderr, "%p Suspended\n", pystate);

    errno = err;
}

void
PyState_MaybeResume(void)
{
    int err = errno;
    PyState *pystate = PyState_Get();

    //fprintf(stderr, "%p Resuming\n", pystate);
    assert(pystate->suspended);
    if (pystate->critical_section == NULL)
        PyThread_lock_acquire(pystate->thread_lock);
    PyThread_lock_acquire(pystate->refowner_lock);
    pystate->suspended = 0;
    pystate->enterframe->locked = 1;
    //fprintf(stderr, "%p Resumed\n", pystate);

    errno = err;
}


/* Do periodic things.  This is called from the main event loop, so we
 * take care to reduce the per-call costs. */
int
PyState_Tick(void)
{
    PyState *pystate = PyState_Get();

    if (pystate->critical_section != NULL)
        Py_FatalError("PyState_Tick called while in critical section");

    if (AO_load_acquire(&world_sleep)) {
        PyThread_lock_acquire(world_wakeup_lock);
        PyLinkedList_Append(&world_wakeup_list, &pystate->world_wakeup_links);
        PyThread_lock_release(world_wakeup_lock);

        PyThread_lock_release(pystate->refowner_lock);
        PyThread_lock_release(pystate->thread_lock);

        PyThread_sem_acquire(pystate->world_wakeup);

        PyThread_lock_acquire(pystate->thread_lock);
        PyThread_lock_acquire(pystate->refowner_lock);
    } else if (AO_load_acquire(&pystate->refowner_waiting_flag)) {
#if 0
        PyState_Suspend();
        PyState_Resume();
#else
        PyThread_lock_release(pystate->refowner_lock);
        PyThread_lock_acquire(pystate->refowner_waiting_lock);
        PyThread_lock_acquire(pystate->refowner_lock);
        PyThread_lock_release(pystate->refowner_waiting_lock);
#endif
    }

#if 0
    if (pystate->small_ticks > 0) {
        pystate->small_ticks--;
        return 0;
    } else {
        PyState_Suspend();
        PyState_Resume();

        pystate->large_ticks++;
        pystate->small_ticks = _Py_CheckInterval; /* XXX use atomic access? */

        return 0;
    }
#endif
    return 0;
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
    PyState *pystate = PyState_Get();

    assert(!pystate->suspended);
    assert(crit->lock != NULL);

    if (pystate->critical_section != NULL &&
                pystate->critical_section->depth <= crit->depth)
        Py_FatalError("PyCritical_Enter called while already in deeper "
            "critical section");

    if (!_PyThread_lock_tryacquire(crit->lock)) {
        PyState_MaybeSuspend();
        PyThread_lock_acquire(crit->lock);
        PyState_MaybeResume();
    }

    assert(crit->prev == NULL);
    crit->prev = pystate->critical_section;
    pystate->critical_section = crit;
}

void
PyCritical_Exit(PyCritical *crit)
{
    PyState *pystate = PyState_Get();

    assert(!pystate->suspended);
    assert(crit->lock != NULL);

    if (pystate->critical_section != crit)
        Py_FatalError("PyCritical_Exit called with wrong critical section");

    pystate->critical_section = crit->prev;
    crit->prev = NULL;

    PyThread_lock_release(crit->lock);
}

void
PyCritical_EnterDummy(PyCritical *crit, Py_ssize_t depth)
{
    PyState *pystate = PyState_Get();

    assert(!pystate->suspended);

    crit->lock = NULL;
    crit->depth = depth;
    crit->prev = NULL;

    if (pystate->critical_section != NULL &&
                pystate->critical_section->depth <= crit->depth)
        Py_FatalError("PyCritical_EnterDummy called while already in "
            "deeper critical section");

    assert(crit->prev == NULL);
    crit->prev = pystate->critical_section;
    pystate->critical_section = crit;
}

void
PyCritical_ExitDummy(PyCritical *crit)
{
    PyState *pystate = PyState_Get();

    assert(!pystate->suspended);
    assert(crit->lock == NULL);

    if (pystate->critical_section != crit)
        Py_FatalError("PyCritical_ExitDummy called with wrong critical "
            "section");

    pystate->critical_section = crit->prev;
    crit->prev = NULL;
}

/* This is just a bodge for deathqueue_wait.  It shouldn't be used in general */
void
_PyCritical_CondWait(PyCritical *crit, PyThread_type_cond cond)
{
    PyState *pystate = PyState_Get();

    assert(!pystate->suspended);

    if (pystate->critical_section != crit)
        Py_FatalError("_PyCritical_CondWait called with wrong "
            "critical section");

    if (crit->prev != NULL)
        Py_FatalError("_PyCritical_CondWait called while in nested "
            "critical section");

    pystate->critical_section = crit->prev;
    crit->prev = NULL;
    PyState_Suspend();

#warning FIXME _PyCritical_CondWait should be interruptible
    PyThread_cond_wait(cond, crit->lock);

    PyState_Resume();
    crit->prev = pystate->critical_section;
    pystate->critical_section = crit;
}


extern PyState * (*pymalloc_pystate_hook)(void);

void
_PyState_InitThreads(void)
{
    world_lock = PyThread_lock_allocate();
    world_wakeup_lock = PyThread_lock_allocate();
    autoTLSkey = PyThread_create_key();
    if (!world_lock || !world_wakeup_lock || !autoTLSkey)
        Py_FatalError("Allocation failed in _PyState_InitThreads");
    pymalloc_pystate_hook = PyState_Get;
}

void
_PyState_ClearThreads(void)
{
    PyState *pystate;

    if (!_PyState_SingleThreaded())
        Py_FatalError("_PyState_ClearThreads should only be called with 1 thread left");

    /* If this blocks, something is seriously wrong */
    PyThread_lock_acquire(world_lock);
    for (pystate = pystate_head; pystate != NULL; pystate = pystate->next)
        _PyState_Clear(pystate);
    PyThread_lock_release(world_lock);
}

void
_PyState_FlushAsyncRefcounts(void)
{
    PyState *pystate;

    /* This should only be called with the world stopped */
    for (pystate = pystate_head; pystate != NULL; pystate = pystate->next)
        _PyGC_AsyncRefcount_Flush(pystate);
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


#ifdef __cplusplus
}
#endif
