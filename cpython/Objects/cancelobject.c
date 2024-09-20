
#include "Python.h"
#include "cancelobject.h"
#include "pystate.h"

#ifdef __cplusplus
extern "C" {
#endif


/* PyCancel */

PyCancelObject *
PyCancel_New(void (*callback)(PyCancelQueue *, void *), void *arg,
        PyState *pystate)
{
    PyCancelObject *scope;

    assert(callback);
    assert(pystate);

    scope = PyObject_New(&PyCancel_Type);
    if (scope == NULL)
        return NULL;
    scope->callback_finished = PyThread_flag_allocate();
    if (!scope->callback_finished) {
        PyObject_Del(scope);
        PyErr_NoMemory();
        return NULL;
    }

    scope->cancelled = 0;
    PyLinkedList_InitNode(&scope->stack_links);
    scope->callback = callback;
    scope->callback_activated = 0;
    scope->arg = arg;
    scope->pystate = pystate;
    PyLinkedList_InitNode(&scope->queue_links);

    return scope;
}

void
PyCancel_Push(PyCancelObject *scope)
{
    PyState *pystate = PyState_Get();
    PyCancelQueue queue;
    PyCancelObject *parent;

    PyCancelQueue_Init(&queue);

    PyCritical_Enter(pystate->cancel_crit);
    assert(PyLinkedList_Detached(&scope->stack_links));

    parent = PyLinkedList_Last(&pystate->cancel_stack);
    if (parent) {
        if (parent->cancelled) {
            /* Enqueue this scope, so that it gets notified its parent
             * is cancelled.  The lock will be released by the queue. */

            PyLinkedList_Append(&queue.list, scope);
            Py_INCREF(scope);
            scope->callback_activated = 1;
        }
    }
    PyLinkedList_Append(&pystate->cancel_stack, scope);
    PyCritical_Exit(pystate->cancel_crit);

    PyCancelQueue_Finish(&queue);
}

void
PyCancel_Pop(PyCancelObject *scope)
{
    PyState *pystate = PyState_Get();
    int activated;

    PyCritical_Enter(pystate->cancel_crit);
    if (PyLinkedList_Last(&pystate->cancel_stack) != scope)
        Py_FatalError("Popping wrong cancel scope");

    PyLinkedList_Remove(&scope->stack_links);

    activated = scope->callback_activated;
    PyCritical_Exit(pystate->cancel_crit);

    if (activated)
        PyThread_flag_wait(scope->callback_finished);
}

static void
cancel_dealloc(PyCancelObject *scope)
{
    assert(PyLinkedList_Detached(&scope->stack_links));
    assert(PyLinkedList_Detached(&scope->queue_links));

    PyThread_flag_free(scope->callback_finished);

    PyObject_Del(scope);
}

static int
cancel_traverse(PyCancelObject *scope, visitproc visit, void *arg)
{
    return 0;
}

static PyObject *
cancel_cancel(PyCancelObject *scope)
{
    PyCancelQueue queue;

    PyCancelQueue_Init(&queue);
    PyCancelQueue_Cancel(&queue, scope);
    PyCancelQueue_Finish(&queue);

    Py_INCREF(Py_None);
    return Py_None;
}

static int
cancel_isshareable(PyObject *obj)
{
    return 1;
}

PyDoc_STRVAR(cancel_doc,
"FIXME");

static PyMethodDef cancel_methods[] = {
    {"cancel", (PyCFunction)cancel_cancel, METH_NOARGS|METH_SHARED, cancel_doc},
    {NULL, NULL}                            /* sentinel */
};

PyTypeObject PyCancel_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "cancel",
    sizeof(PyCancelObject),
    0,
    (destructor)cancel_dealloc,             /* tp_dealloc */
    0,                                      /* tp_print */
    0,                                      /* tp_getattr */
    0,                                      /* tp_setattr */
    0,                                      /* tp_compare */
    0,                                      /* tp_repr */
    0,                                      /* tp_as_number */
    0,                                      /* tp_as_sequence */
    0,                                      /* tp_as_mapping */
    0,                                      /* tp_hash */
    0,                                      /* tp_call */
    0,                                      /* tp_str */
    PyObject_GenericGetAttr,                /* tp_getattro */
    0,                                      /* tp_setattro */
    0,                                      /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                      /* tp_doc */
    (traverseproc)cancel_traverse,          /* tp_traverse */
    0,                                      /* tp_clear */
    0,                                      /* tp_richcompare */
    0,                                      /* tp_weaklistoffset */
    0,                                      /* tp_iter */
    0,                                      /* tp_iternext */
    cancel_methods,                         /* tp_methods */
    0,                                      /* tp_members */
    0,                                      /* tp_getset */
    0,                                      /* tp_base */
    0,                                      /* tp_dict */
    0,                                      /* tp_descr_get */
    0,                                      /* tp_descr_set */
    0,                                      /* tp_dictoffset */
    0,                                      /* tp_init */
    0,                                      /* tp_new */
    0,                                      /* tp_is_gc */
    0,                                      /* tp_bases */
    0,                                      /* tp_mro */
    0,                                      /* tp_cache */
    0,                                      /* tp_subclasses */
    0,                                      /* tp_weaklist */
    cancel_isshareable,                     /* tp_isshareable */
};


void
PyCancelQueue_Init(PyCancelQueue *queue)
{
    PyLinkedList_InitBase(&queue->list,
        offsetof(PyCancelObject, queue_links));
}

/* This scope was cancelled.  It is *NOT* added to the queue - only its
 * children are, so that they can be notified */
void
PyCancelQueue_Cancel(PyCancelQueue *queue, PyCancelObject *scope)
{
    PyCancelObject *child;

    PyCritical_Enter(scope->pystate->cancel_crit);
    if (!scope->cancelled && PyLinkedList_Detached(&scope->stack_links))
        scope->cancelled = 1;
    else if (!scope->cancelled) {
        scope->cancelled = 1;

        child = PyLinkedList_After(&scope->pystate->cancel_stack, scope);
        if (child) {
            /* Ask the queue to notify the child that we got cancelled.
             * Their lock is left acquired, and will be released by the
             * queue after it finishes. */

            PyLinkedList_Append(&queue->list, child);
            Py_INCREF(child);
            assert(!child->callback_activated);
            child->callback_activated = 1;
        }
    }
    PyCritical_Exit(scope->pystate->cancel_crit);
}

void
PyCancelQueue_Finish(PyCancelQueue *queue)
{
    while (!PyLinkedList_Empty(&queue->list)) {
        PyCancelObject *scope = PyLinkedList_First(&queue->list);
        PyLinkedList_Remove(&scope->queue_links);

        scope->callback(queue, scope->arg);
        PyThread_flag_set(scope->callback_finished);

        Py_DECREF(scope);
    }
}


/* Returns 0 if we're not in an cancelled state,
 * 1 and raises an exception if we are */
int
PyCancel_CheckCancelled(void)
{
    PyState *pystate = PyState_Get();
    PyCancelObject *scope = PyLinkedList_Last(&pystate->cancel_stack);

    PyCritical_Enter(pystate->cancel_crit);
    if (scope->cancelled) {
        PyCritical_Exit(pystate->cancel_crit);
        PyErr_SetNone(PyExc_Cancelled);
        return 1;
    }
    PyCritical_Exit(pystate->cancel_crit);
    return 0;
}


#warning blah blah signal cancel API should have retries
/* XXX FIXME the signal handler thread should be used to send the signal multiple times, if the first attempt doesn't work.  Actually, the signal handler itself should mark it as received, and the retry should check if the call actually stopped.
 * Or should whoever's running the cancel callback do the retry?  Just have them stick around until the cancellation process finishes.
 */
void
PyCancel_SignalEnter(void)
{
    // wipe state
    // allocate lock
    // allocate semaphore
    // setup cancel callback
    // PyState_Suspend();
    // grab lock
    // mark as active (cancel callback may now actually send signal)
    // release lock
    Py_FatalError("PyCancel_SignalEnter not implemented");
}

void
PyCancel_SignalExit(void)
{
    // grab lock
    // mark as inactive
    // post semaphore
    // XXX FIXME blah, I need more work to coordinate teardown with the cancel callback
    // release lock
    // PyState_Resume();
    // Remove cancel callback
    // ??? release lock
    // ??? release semaphore
    // I need to track that the cancel callback has started, and if so I have to wake them up and then either wait for them to signal me that they've exited, or let them do the cleanup.  The number of context switches seems the same either way, and I think it'd be simpler if it was always this function doing the cleanup.
    // XXX FIXME this should also save errno
    Py_FatalError("PyCancel_SignalExit not implemented");
}

int
PyCancel_Poll(int fd)
{
    Py_FatalError("PyCancel_Poll not implemented");
}

#if 0
int
PyCancel_WaitForMultipleObjects(...)
{
    Py_FatalError("PyCancel_WaitForMultipleObjects not implemented");
}
#endif

int
PyCancel_Sleep(double secs)
{
    Py_FatalError("PyCancel_Sleep not implemented");
}

/* XXX FIXME consider replacing this with Sleep(Inf) */
int
PyCancel_SleepForever(void)
{
    Py_FatalError("PyCancel_SleepForever not implemented");
}


#ifdef __cplusplus
}
#endif
