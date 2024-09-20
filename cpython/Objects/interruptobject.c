
#include "Python.h"
#include "interruptobject.h"
#include "pystate.h"

#ifdef __cplusplus
extern "C" {
#endif


/* PyInterrupt */

PyInterruptObject *
PyInterrupt_New(void (*callback_c)(struct _PyInterruptQueue *, void *),
		void *arg, PyObject *callback_python)
{
	PyInterruptObject *point;

	if (!callback_c && !callback_python)
		Py_FatalError("PyInterrupt_New called with no callback");
	if (callback_c && callback_python)
		Py_FatalError("PyInterrupt_New called with both callbacks");

	point = PyObject_NEW(PyInterruptObject, &PyInterrupt_Type);
	if (point == NULL)
		return NULL;
	point->lock = PyThread_lock_allocate();
	if (!point->lock) {
		PyObject_DEL(point);
		PyErr_NoMemory();
		return NULL;
	}

	Py_XINCREF(callback_python);
	point->interrupted = 0;
	point->parent = NULL;
	point->child = NULL;
	point->notify_parent_int_c = callback_c;
	point->arg = arg;
	point->notify_parent_int_python = callback_python;
	point->next = NULL;

	return point;
}

void
PyInterrupt_Push(PyInterruptObject *point)
{
	int run_callbacks = 0;
	PyThreadState *tstate = PyThreadState_Get();

	assert(point->parent == NULL);
	assert(point->child == NULL);

	point->parent = tstate->interrupt_point;
	if (point->parent) {
		PyThread_lock_acquire(point->parent->lock);
		assert(point->parent->child == NULL);
		point->parent->child = point;
		if (point->parent->interrupted) {
			run_callbacks = 1;
		}
		PyThread_lock_release(point->parent->lock);
	}
	tstate->interrupt_point = point;

	if (run_callbacks) {
		PyInterruptQueue queue;

		PyInterruptQueue_Init(&queue);
		PyInterruptQueue_AddFromParent(&queue, point);
		PyInterruptQueue_Finish(&queue);
	}
}

void
PyInterrupt_Pop(PyInterruptObject *point)
{
	PyThreadState *tstate = PyThreadState_Get();

	assert(point->child == NULL);
	if (point != tstate->interrupt_point)
		Py_FatalError("Popping wrong interrupt point");

	tstate->interrupt_point = point->parent;
	if (point->parent != NULL) {
		PyThread_lock_acquire(point->parent->lock);
		assert(point->parent->child == point);
		point->parent->child = NULL;
		PyThread_lock_release(point->parent->lock);
		point->parent = NULL;
	}

	PyThread_lock_acquire(point->lock);
	/* Other threads may still reference us, but they'll now have no effect */
	PyThread_lock_release(point->lock);
}

static void
interrupt_dealloc(PyInterruptObject *point)
{
	assert(point->notify_parent_int_c || point->notify_parent_int_python);
	assert(!(point->notify_parent_int_c && point->notify_parent_int_python));
	assert(point->parent == NULL);
	assert(point->child == NULL);
	assert(point->next == NULL);

	Py_XDECREF(point->notify_parent_int_python);
	PyThread_lock_free(point->lock);
}

static int
interrupt_traverse(PyInterruptObject *point, visitproc visit, void *arg)
{
	Py_VISIT(point->notify_parent_int_python);
	return 0;
}

static PyObject *
interrupt_interrupt(PyInterruptObject *point)
{
	PyInterruptQueue queue;

	PyInterruptQueue_Init(&queue);
	PyInterruptQueue_Add(&queue, point);
	PyInterruptQueue_Finish(&queue);

	Py_INCREF(Py_None);
	return Py_None;
}

static int
interrupt_isshareable(PyObject *obj)
{
	return 1;
}

PyDoc_STRVAR(interrupt_doc,
"FIXME");

static PyMethodDef interrupt_methods[] = {
	{"interrupt", (PyCFunction)interrupt_interrupt, METH_NOARGS|METH_SHARED, interrupt_doc},
 	{NULL,		NULL}		/* sentinel */
};

PyTypeObject PyInterrupt_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"Interrupt",
	sizeof(PyInterruptObject),
	0,
	(destructor)interrupt_dealloc,		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
 	0,					/* tp_doc */
 	(traverseproc)interrupt_traverse,	/* tp_traverse */
	0,					/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	0,					/* tp_iter */
	0,					/* tp_iternext */
	interrupt_methods,			/* tp_methods */
	0,					/* tp_members */
	0,					/* tp_getset */
	0,					/* tp_base */
	0,					/* tp_dict */
	0,					/* tp_descr_get */
	0,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	0,					/* tp_init */
	0,					/* tp_new */
	0,					/* tp_is_gc */
	0,					/* tp_bases */
	0,					/* tp_mro */
	0,					/* tp_cache */
	0,					/* tp_subclasses */
	0,					/* tp_weaklist */
	interrupt_isshareable,			/* tp_isshareable */
};


void
PyInterruptQueue_Init(PyInterruptQueue *queue)
{
	queue->head = NULL;
	queue->tail = NULL;
}

void
PyInterruptQueue_Add(PyInterruptQueue *queue, PyInterruptObject *point)
{
	PyThread_lock_acquire(point->lock);
	if (!point->interrupted) {
		point->interrupted = 1;

		if (point->child)
			PyInterruptQueue_AddFromParent(queue, point->child);
	}
	PyThread_lock_release(point->lock);
}

void
PyInterruptQueue_AddFromParent(PyInterruptQueue *queue, PyInterruptObject *point)
{
	PyThread_lock_acquire(point->lock);
	assert(point->next == NULL);
	if (point->parent != NULL) {
		if (point->notify_parent_int_c) {
			point->notify_parent_int_c(queue, point->arg);
		} else {
			assert(point->notify_parent_int_python);
			if (queue->tail) {
				queue->tail->next = point;
				queue->tail = point;
			} else {
				queue->head = point;
				queue->tail = point;
			}
			Py_INCREF(point);
		}
	}
	PyThread_lock_release(point->lock);
}

void
PyInterruptQueue_Finish(PyInterruptQueue *queue)
{
	while (queue->head) {
		PyObject *result;
		PyInterruptObject *point = queue->head;
		queue->head = point->next;
		point->next = NULL;

		/* XXX FIXME returning True should implicitly call
		 * .interrupt().  Maybe? */
		result = PyObject_CallObject(point->notify_parent_int_python, NULL);
		if (result == NULL)
			PyErr_WriteUnraisable(point->notify_parent_int_python);
		else
			Py_DECREF(result);

		Py_DECREF(point);
	}
}


/* Returns 0 if we're not in an interrupted state,
 * 1 and raises an exception if we are */
int
PyCancel_Check(void)
{
    PyThreadState *tstate = PyThreadState_Get();

    PyThread_lock_acquire(tstate->interrupt_point->lock);
    if (tstate->interrupt_point->interrupted) {
        PyThread_lock_release(tstate->interrupt_point->lock);
        PyErr_SetNone(PyExc_Interrupted);
        return 1;
    }
    PyThread_lock_release(tstate->interrupt_point->lock);
    return 0;
}


#warning blah blah signal interrupt API should have retries
/* XXX FIXME the signal handler thread should be used to send the signal multiple times, if the first attempt doesn't work.  Actually, the signal handler itself should mark it as received, and the retry should check if the call actually stopped.
 * Or should whoever's running the interrupt callback do the retry?  Just have them stick around until the interruption process finishes.
 */
void
PyCancel_SignalEnter(void)
{
    // wipe state
    // allocate lock
    // allocate semaphore
    // setup interrupt callback
    // PyState_Suspend();
    // grab lock
    // mark as active (interrupt callback may now actually send signal)
    // release lock
    Py_FatalError("PyCancel_SignalEnter not implemented");
}

void
PyCancel_SignalExit(void)
{
    // grab lock
    // mark as inactive
    // post semaphore
    // XXX FIXME blah, I need more work to coordinate teardown with the interrupt callback
    // release lock
    // PyState_Resume();
    // Remove interrupt callback
    // ??? release lock
    // ??? release semaphore
    // I need to track that the interrupt callback has started, and if so I have to wake them up and then either wait for them to signal me that they've exited, or let them do the cleanup.  The number of context switches seems the same either way, and I think it'd be simpler if it was always this function doing the cleanup.
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
