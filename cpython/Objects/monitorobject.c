
#include "Python.h"
#include "ceval.h"
#include "monitorobject.h"

/* MonitorMeta methods */

static void
MonitorMeta_dealloc(PyTypeObject *self)
{
	PyObject_DEL(self);
}

static PyObject *
MonitorMeta_call(PyTypeObject *self, PyObject *args, PyObject *kwds)
{
	PyObject *monitorspace;
	PyObject *super;
	PyObject *nextmethod;
	PyObject *enter;
	Py_ssize_t size, i;
	PyObject *built_args;
	PyObject *result;

	assert(PyTuple_Check(args));

	/* This can all be summed up as:
	   monitorspace = MonitorSpace()
	   nextmethod = super(MonitorMeta, self).__call__
	   return monitorspace.enter(nextmethod, *args, **kwargs)
	 */

	monitorspace = PyObject_CallObject((PyObject *)&PyMonitorSpace_Type, NULL);
	if (monitorspace == NULL)
		return NULL;

	super = PyEval_CallFunction((PyObject *)&PySuper_Type, "OO",
		&PyMonitorMeta_Type, self);
	if (super == NULL) {
		Py_DECREF(monitorspace);
		return NULL;
	}

	nextmethod = PyObject_GetAttrString(super, "__call__");
	Py_DECREF(super);
	if (nextmethod == NULL) {
		Py_DECREF(monitorspace);
		return NULL;
	}

	size = PyTuple_Size(args) + 1;
	built_args = PyTuple_New(size);
	if (built_args == NULL) {
		Py_DECREF(monitorspace);
		Py_DECREF(nextmethod);
		return NULL;
	}
	PyTuple_SET_ITEM(built_args, 0, nextmethod); /* Steals our reference */
	nextmethod = NULL;
	for (i = 1; i < size; i++) {
		PyObject *item = PyTuple_GET_ITEM(args, i-1);
		Py_INCREF(item);
		PyTuple_SET_ITEM(built_args, i, item);
	}

	enter = PyObject_GetAttrString(monitorspace, "enter");
	if (enter == NULL) {
		Py_DECREF(monitorspace);
		Py_DECREF(built_args);
		return NULL;
	}

	result = PyEval_CallObjectWithKeywords(enter, built_args, kwds);

	Py_DECREF(monitorspace);
	Py_DECREF(built_args);
	Py_DECREF(enter);

	return result;
}

static int
MonitorMeta_traverse(PyTypeObject *self, visitproc visit, void *arg)
{
	return 0;
}

PyTypeObject PyMonitorMeta_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"_threadtoolsmodule.MonitorMeta",	/*tp_name*/
	sizeof(PyHeapTypeObject),	/*tp_basicsize*/
	0/*sizeof(PyMemberDef)*/,	/*tp_itemsize*/
	(destructor)MonitorMeta_dealloc,	/*tp_dealloc*/
	0,			/*tp_print*/
	0,			/*tp_getattr*/
	0,			/*tp_setattr*/
	0,			/*tp_compare*/
	0,			/*tp_repr*/
	0,			/*tp_as_number*/
	0,			/*tp_as_sequence*/
	0,			/*tp_as_mapping*/
	0,			/*tp_hash*/
	(ternaryfunc)MonitorMeta_call,	/*tp_call*/
	0,			/*tp_str*/
	PyObject_GenericGetAttr,	/*tp_getattro*/
	0,			/*tp_setattro*/
	0,			/*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
		Py_TPFLAGS_BASETYPE |
		Py_TPFLAGS_SHAREABLE,	/*tp_flags*/
	0,			/*tp_doc*/
	(traverseproc)MonitorMeta_traverse,	/*tp_traverse*/
	0,			/*tp_clear*/
	0,			/*tp_richcompare*/
	0,			/*tp_weaklistoffset*/
	0,			/*tp_iter*/
	0,			/*tp_iternext*/
	0,			/*tp_methods*/
	0,			/*tp_members*/
	0,			/*tp_getset*/
	0,			/*tp_base*/
	0,			/*tp_dict*/
	0,			/*tp_descr_get*/
	0,			/*tp_descr_set*/
	0,			/*tp_dictoffset*/
	0,			/*tp_init*/
	0,			/*tp_new*/
};
/* --------------------------------------------------------------------- */


/* Monitor methods */

static void
Monitor_dealloc(PyMonitorObject *self)
{
	PyObject *monitorspace = self->mon_monitorspace;
	PyObject_DEL(self);
	Py_DECREF(monitorspace);
}

static PyObject *
Monitor_enter(PyMonitorObject *self, PyObject *args, PyObject *kwds)
{
	PyObject *monitorspace_enter;
	PyObject *result;

	monitorspace_enter = PyObject_GetAttrString(self->mon_monitorspace, "enter");
	if (monitorspace_enter == NULL)
	    return NULL;

	result = PyEval_CallObjectWithKeywords(monitorspace_enter, args, kwds);
	Py_DECREF(monitorspace_enter);
	return result;
}

static int
Monitor_traverse(PyMonitorObject *self, visitproc visit, void *arg)
{
	Py_VISIT(self->mon_monitorspace);
	return 0;
}

static PyObject *
Monitor_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	PyObject *self;

	assert(type != NULL);
	self = PyObject_New(type);
	if (self != NULL) {
		PyMonitorObject *x = (PyMonitorObject *)self;
		x->mon_monitorspace = PyMonitorSpace_GetCurrent();
		if (x->mon_monitorspace == NULL) {
			PyObject_Del(self);
			return NULL;
		}
	}
	return self;
}

static int
Monitor_isshareable (PyObject *self)
{
	return 1;
}

PyDoc_STRVAR(Monitor_enter__doc__, "enter(func, *args, **kwargs) -> object");

static PyMethodDef Monitor_methods[] = {
	{"enter",	(PyCFunction)Monitor_enter,	METH_VARARGS | METH_KEYWORDS,
		Monitor_enter__doc__},
	{NULL,		NULL}		/* sentinel */
};

PyTypeObject PyMonitor_Type = {
	PyVarObject_HEAD_INIT(&PyMonitorMeta_Type, 0)
	"_threadtoolsmodule.Monitor",	/*tp_name*/
	sizeof(PyMonitorObject),	/*tp_basicsize*/
	0,			/*tp_itemsize*/
	(destructor)Monitor_dealloc,	/*tp_dealloc*/
	0,			/*tp_print*/
	0,			/*tp_getattr*/
	0,			/*tp_setattr*/
	0,			/*tp_compare*/
	0,			/*tp_repr*/
	0,			/*tp_as_number*/
	0,			/*tp_as_sequence*/
	0,			/*tp_as_mapping*/
	0,			/*tp_hash*/
	0,			/*tp_call*/
	0,			/*tp_str*/
	PyObject_GenericGetAttr,	/*tp_getattro*/
	0,			/*tp_setattro*/
	0,			/*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
		Py_TPFLAGS_BASETYPE | Py_TPFLAGS_MONITOR_SUBCLASS |
		Py_TPFLAGS_SHAREABLE,	/*tp_flags*/
	0,			/*tp_doc*/
	(traverseproc)Monitor_traverse,	/*tp_traverse*/
	0,			/*tp_clear*/
	0,			/*tp_richcompare*/
	0,			/*tp_weaklistoffset*/
	0,			/*tp_iter*/
	0,			/*tp_iternext*/
	Monitor_methods,	/*tp_methods*/
	0,			/*tp_members*/
	0,			/*tp_getset*/
	0,			/*tp_base*/
	0,			/*tp_dict*/
	0,			/*tp_descr_get*/
	0,			/*tp_descr_set*/
	0,			/*tp_dictoffset*/
	0,			/*tp_init*/
	Monitor_new,		/*tp_new*/
	0,			/*tp_is_gc*/
	0,			/*tp_bases*/
	0,			/*tp_mro*/
	0,			/*tp_cache*/
	0,			/*tp_subclasses*/
	0,			/*tp_weaklist*/
	Monitor_isshareable,	/*tp_isshareable*/
};
/* --------------------------------------------------------------------- */


/* MonitorSpace methods */

static void
waitqueue_push(PyMonitorSpaceObject *queue, PyState *pystate)
{
	if (queue->first_waiter) {
		pystate->lockwait_prev = queue->last_waiter;
		pystate->lockwait_next = NULL;
		queue->last_waiter->lockwait_next = pystate;
		queue->last_waiter = pystate;
	} else {
		pystate->lockwait_prev = NULL;
		pystate->lockwait_next = NULL;
		queue->first_waiter = pystate;
		queue->last_waiter = pystate;
	}
}

static void
waitqueue_pop(PyMonitorSpaceObject *queue, PyState *pystate)
{
	if (pystate->lockwait_prev != NULL)
		pystate->lockwait_prev->lockwait_next = pystate->lockwait_next;
	if (pystate->lockwait_next != NULL)
		pystate->lockwait_next->lockwait_prev = pystate->lockwait_prev;

	if (queue->first_waiter == pystate)
		queue->first_waiter = pystate->lockwait_next;
	if (queue->last_waiter == pystate)
		queue->last_waiter = pystate->lockwait_prev;

	pystate->lockwait_next = NULL;
	pystate->lockwait_prev = NULL;
}

static int
lock_enter(PyMonitorSpaceObject *self)
{
	PyState *pystate = PyState_Get();
	assert(pystate->active_lock == NULL);

	pystate->active_lock = self;
	PyState_Suspend();
	PyThread_lock_acquire(self->lock);

	if (self->lock_holder == NULL)
		self->lock_holder = pystate;
	else {
		waitqueue_push(self, pystate);
		while (self->lock_holder != NULL)
			PyThread_cond_wait(pystate->lockwait_cond, self->lock);
		waitqueue_pop(self, pystate);
		self->lock_holder = pystate;
	}

	PyThread_lock_release(self->lock);
	PyState_Resume();
	pystate->active_lock = NULL;

	return 0;
}

static int
lock_exit(PyMonitorSpaceObject *self)
{
	PyState *pystate = PyState_Get();
	assert(pystate->active_lock == NULL);

	pystate->active_lock = self;
	PyState_Suspend();
	PyThread_lock_acquire(self->lock);

	assert(self->lock_holder == pystate);
	self->lock_holder = NULL;
	if (self->first_waiter) {
		/* Ensure there's at least one thread that's awake. */
		PyThread_cond_wakeone(self->first_waiter->lockwait_cond);
	}

	PyThread_lock_release(self->lock);
	PyState_Resume();
	pystate->active_lock = NULL;

	return 0;
}

static PyObject *
monitorspace_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	PyObject *self;

	assert(type != NULL);
	self = PyObject_New(type);
	if (self != NULL) {
		PyMonitorSpaceObject *x = (PyMonitorSpaceObject *)self;
		x->lock = PyThread_lock_allocate();
		if (x->lock == NULL) {
			PyObject_Del(self);
			PyErr_SetString(PyExc_RuntimeError, "can't allocate lock");
			return NULL;
		}
		x->lock_holder = NULL;
		x->first_waiter = NULL;
		x->last_waiter = NULL;
	}
	return self;
}

static void
monitorspace_dealloc(PyMonitorSpaceObject *self)
{
	assert(self->lock_holder == NULL);
	assert(self->first_waiter == NULL);
	assert(self->last_waiter == NULL);
	PyThread_lock_free(self->lock);
	PyObject_DEL(self);
}

static PyObject *
monitorspace_enter(PyMonitorSpaceObject *self, PyObject *args, PyObject *kwds)
{
	PyObject *func;
	PyObject *smallargs;
	PyObject *result;
	PyMonitorSpaceFrame frame;
	PyState *pystate = PyState_Get();

	if (PyTuple_Size(args) < 1) {
		PyErr_SetString(PyExc_TypeError,
			"Monitor.enter() needs a function to be called");
		return NULL;
	}

	func = PyTuple_GetItem(args, 0);

	if (!PyObject_IsShareable(func)) {
		PyErr_Format(PyExc_TypeError,
			"Function argument must be shareable, '%s' object "
			"is not", func->ob_type->tp_name);
		return NULL;
	}

	smallargs = PyTuple_GetSlice(args, 1, PyTuple_Size(args));
	if (smallargs == NULL) {
		return NULL;
	}

	if (!PyArg_RequireShareable("Monitor.enter", smallargs, kwds)) {
		Py_DECREF(smallargs);
		return NULL;
	}

	if (pystate->critical_section != NULL)
		Py_FatalError("Cannot enter monitor while in a critical section");

	lock_enter(self);
	frame.prevframe = pystate->monitorspace_frame;
	frame.monitorspace = self;
	pystate->monitorspace_frame = &frame;

	result = PyEval_CallObjectWithKeywords(func, smallargs, kwds);
	if (!PyArg_RequireShareableReturn("Monitor.enter", func, result)) {
		Py_XDECREF(result);
		result = NULL;
	}
	Py_DECREF(smallargs);

	assert(pystate->monitorspace_frame == &frame);
	assert(frame.monitorspace == self);

	pystate->monitorspace_frame = frame.prevframe;
	lock_exit(self);

	return result;
}

int
PyMonitorSpace_IsCurrent(struct _PyMonitorSpaceObject *monitorspace)
{
	PyState *pystate = PyState_Get();

	assert(monitorspace != NULL);
	return pystate->monitorspace_frame->monitorspace == monitorspace;
}

/* Returns a NEW reference */
PyObject *
PyMonitorSpace_GetCurrent(void)
{
    PyState *pystate = PyState_Get();
    PyObject *monitorspace = (PyObject *)pystate->monitorspace_frame->monitorspace;

    if (monitorspace == NULL) {
        monitorspace = PyObject_CallObject((PyObject *)&PyMonitorSpace_Type, NULL);
        if (monitorspace == NULL)
            return NULL;
        assert(pystate->monitorspace_frame->monitorspace == NULL);
        pystate->monitorspace_frame->monitorspace =
            (struct _PyMonitorSpaceObject *)monitorspace;
    }

    Py_INCREF(monitorspace);
    return monitorspace;
}

static int
monitorspace_isshareable (PyObject *self)
{
	return 1;
}

PyDoc_STRVAR(monitorspace_enter__doc__, "enter(func, *args, **kwargs) -> object");

static PyMethodDef monitorspace_methods[] = {
	{"enter",	(PyCFunction)monitorspace_enter,	METH_VARARGS | METH_KEYWORDS,
		monitorspace_enter__doc__},
	{NULL,		NULL}		/* sentinel */
};

PyTypeObject PyMonitorSpace_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"_threadtoolsmodule.MonitorSpace",	/*tp_name*/
	sizeof(PyMonitorSpaceObject),	/*tp_basicsize*/
	0,			/*tp_itemsize*/
	(destructor)monitorspace_dealloc,	/*tp_dealloc*/
	0,			/*tp_print*/
	0,			/*tp_getattr*/
	0,			/*tp_setattr*/
	0,			/*tp_compare*/
	0,			/*tp_repr*/
	0,			/*tp_as_number*/
	0,			/*tp_as_sequence*/
	0,			/*tp_as_mapping*/
	0,			/*tp_hash*/
	0,			/*tp_call*/
	0,			/*tp_str*/
	PyObject_GenericGetAttr,	/*tp_getattro*/
	0,			/*tp_setattro*/
	0,			/*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
		Py_TPFLAGS_SHAREABLE,	/*tp_flags*/
	0,			/*tp_doc*/
	0,			/*tp_traverse*/
	0,			/*tp_clear*/
	0,			/*tp_richcompare*/
	0,			/*tp_weaklistoffset*/
	0,			/*tp_iter*/
	0,			/*tp_iternext*/
	monitorspace_methods,	/*tp_methods*/
	0,			/*tp_members*/
	0,			/*tp_getset*/
	0,			/*tp_base*/
	0,			/*tp_dict*/
	0,			/*tp_descr_get*/
	0,			/*tp_descr_set*/
	0,			/*tp_dictoffset*/
	0,			/*tp_init*/
	monitorspace_new,		/*tp_new*/
	0,			/*tp_is_gc*/
	0,			/*tp_bases*/
	0,			/*tp_mro*/
	0,			/*tp_cache*/
	0,			/*tp_subclasses*/
	0,			/*tp_weaklist*/
	monitorspace_isshareable,	/*tp_isshareable*/
};
