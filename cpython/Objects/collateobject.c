
#include "Python.h"
#include "ceval.h"
#include "interruptobject.h"
#include "collateobject.h"


/* Collate methods */

static void collate_baseinterrupt(struct _PyInterruptQueue *queue, void *arg);
static void collatechild_interrupt(PyInterruptQueue *queue, void *arg);
static int collate_add_common(PyCollateObject *self, PyObject *args,
	PyObject *kwds, char *name, int saveresult);
static void collate_threadbootstrap(void *arg);
static int collate_spawn_thread(PyCollateObject *self, PyObject *func,
	PyObject *args, PyObject *kwds, char *name, int save_result);

static void CollateChild_Delete(PyCollateChild *child);
static void CollateChild_DeleteWithResult(PyCollateChild *child);
static void CollateChild_DeleteWithFailure(PyCollateChild *child);
static void _push_child(PyCollateObject *self, PyCollateChild *child);
static void _pop_child(PyCollateObject *self, PyCollateChild *child);

static PyObject *Collate_getresults(PyCollateObject *self);
static void Collate_raisefailure(PyCollateObject *self);

static PyObject *
Collate_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	PyCollateObject *self;

	assert(type != NULL);

        self = PyObject_NEW(PyCollateObject, type);
	if (self == NULL)
		return NULL;

	self->col_lock = PyThread_lock_allocate();
	if (self->col_lock == NULL) {
		PyObject_DEL(self);
		PyErr_SetString(PyExc_RuntimeError, "can't allocate lock");
		return NULL;
	}

	self->col_state = COLLATE_NEW;
	self->col_ownerthread = NULL;
	self->col_threads = NULL;
	self->col_head = NULL;
	self->col_tail = NULL;

	self->col_threadcount = 0;
	self->col_nothreads = PyThread_sem_allocate(1);
	if (self->col_nothreads == NULL) {
		PyThread_lock_free(self->col_lock);
		PyObject_DEL(self);
		PyErr_SetString(PyExc_RuntimeError, "can't allocate semaphore");
		return NULL;
	}
	self->col_baseinterrupt = NULL;

	self->col_interrupting = 0;
	self->col_resultcount = 0;
	self->col_failurecount = 0;

	return (PyObject *)self;
}

static void
Collate_dealloc(PyCollateObject *self)
{
	if (self->col_state != COLLATE_NEW && self->col_state != COLLATE_DEAD)
		Py_FatalError("Invalid state in Collate_dealloc()");
	if (self->col_threadcount != 0)
		Py_FatalError("Remaining threads in Collate_dealloc()");

	PyThread_lock_free(self->col_lock);
	PyThread_sem_free(self->col_nothreads);

	assert(self->col_baseinterrupt == NULL);

	while (self->col_head) {
		PyCollateChild *child = self->col_head;
		_pop_child(self, child);
		CollateChild_DeleteWithResult(child);
		self->col_resultcount--;
	}

	assert(self->col_resultcount == 0);
	assert(self->col_failurecount == 0);

	PyObject_DEL(self);
}

static PyCollateChild *
CollateChild_New(PyCollateObject *collate, PyObject *func,
		PyObject *args, PyObject *kwds)
{
	PyCollateChild *child;
	
	child = malloc(sizeof(PyCollateChild));
	if (child == NULL) {
		PyErr_NoMemory();
		return NULL;
	}
	child->interp = PyThreadState_Get()->interp;
	child->tstate = NULL;
	child->interrupt_point = PyInterrupt_New(collatechild_interrupt,
			NULL, NULL);
	if (child->interrupt_point == NULL) {
		free(child);
		PyErr_NoMemory();
		return NULL;
	}
	child->collate = collate;

	Py_INCREF(func);
	child->func = func;
	Py_INCREF(args);
	child->args = args;
	Py_XINCREF(kwds);
	child->kwds = kwds;

	child->save_result = 0;
	child->result = NULL;
	child->failure.e_type = NULL;
	child->failure.e_value = NULL;
	child->failure.e_traceback = NULL;
	child->prev = NULL;
	child->next = NULL;

	return child;
}

static void
CollateChild_Delete(PyCollateChild *child)
{
	Py_DECREF(child->interrupt_point);

	assert(child->prev == NULL);
	assert(child->next == NULL);

	assert(child->result == NULL);

	assert(child->failure.e_type == NULL);
	assert(child->failure.e_value == NULL);
	assert(child->failure.e_traceback == NULL);

	Py_XDECREF(child->func);
	Py_XDECREF(child->args);
	Py_XDECREF(child->kwds);

	free(child);
}

static void
CollateChild_DeleteWithResult(PyCollateChild *child)
{
	Py_DECREF(child->interrupt_point);

	assert(child->prev == NULL);
	assert(child->next == NULL);

	Py_XDECREF(child->result);

	assert(child->failure.e_type == NULL);
	assert(child->failure.e_value == NULL);
	assert(child->failure.e_traceback == NULL);

	Py_XDECREF(child->func);
	Py_XDECREF(child->args);
	Py_XDECREF(child->kwds);

	free(child);
}

static void
CollateChild_DeleteWithFailure(PyCollateChild *child)
{
	Py_DECREF(child->interrupt_point);

	assert(child->prev == NULL);
	assert(child->next == NULL);

	assert(child->result == NULL);

	Py_XDECREF(child->failure.e_type);
	Py_XDECREF(child->failure.e_value);
	Py_XDECREF(child->failure.e_traceback);

	Py_XDECREF(child->func);
	Py_XDECREF(child->args);
	Py_XDECREF(child->kwds);

	free(child);
}

static void
collatechild_interrupt(PyInterruptQueue *queue, void *arg)
{
	Py_FatalError("collatechild_interrupt called");
	/* XXX FIXME */
}

static PyObject *
Collate___enter__(PyCollateObject *self)
{
	PyInterruptObject *baseinterrupt;
	PyCollateChild *mainchild = CollateChild_New(self, Py_None, Py_None, Py_None);
	if (mainchild == NULL)
		return NULL;

	baseinterrupt = PyInterrupt_New(collate_baseinterrupt, self, NULL);
	if (baseinterrupt == NULL) {
		CollateChild_Delete(mainchild);
		return NULL;
	}

	/* Begin unlocked region */
	PyState_Suspend();
	PyThread_lock_acquire(self->col_lock);

	if (self->col_state != COLLATE_NEW) {
		PyThread_lock_release(self->col_lock);
		PyState_Resume();
		/* End unlocked region */

		Py_DECREF(baseinterrupt);
		CollateChild_Delete(mainchild);
		PyErr_SetString(PyExc_TypeError, "collate.__enter__() "
			"called in wrong state");
		return NULL;
	}

	self->col_mainthread = mainchild;
	_push_child(self, mainchild);
	/* XXX setup interrupt stack for current thread */
	self->col_baseinterrupt = baseinterrupt;
	PyInterrupt_Push(self->col_baseinterrupt);

	self->col_state = COLLATE_ALIVE;

	PyThread_lock_release(self->col_lock);
	PyState_Resume();
	/* End unlocked region */

	Py_INCREF(self);
	return (PyObject *)self;
}

static PyObject *
Collate___exit__(PyCollateObject *self, PyObject *args)
{
	PyInterruptQueue queue;
	int run_queue = 0;
	PyExcBox box;
	int delete_child = 0;

	if (!PyArg_ParseTuple(args, "OOO", &box.e_type, &box.e_value, &box.e_traceback))
		Py_FatalError("Collate.__exit__() got bad arguments");

	if (box.e_type == Py_None) {
		box.e_type = NULL;
		box.e_value = NULL;
		box.e_traceback = NULL;
	} else {
		//printf("snoopexit1: %d\n", Py_RefcntSnoop(box.e_type));
		//printf("snoopexit1: %d\n", Py_RefcntSnoop(box.e_value));
		//printf("snoopexit1: %d\n", Py_RefcntSnoop(box.e_traceback));
		Py_INCREF(box.e_type);
		Py_INCREF(box.e_value);
		Py_INCREF(box.e_traceback);
	}

	/* Begin unlocked region */
	PyState_Suspend();
	PyThread_lock_acquire(self->col_lock);

	assert(self->col_state == COLLATE_ALIVE);
	self->col_state = COLLATE_DYING;

	/* XXX pop interrupt stack for current thread */
	if (box.e_type != NULL) {
		self->col_failurecount++;
		self->col_mainthread->failure = box;
		if (self->col_failurecount == 1) {
			PyCollateChild *child;

			PyInterruptQueue_Init(&queue);
			for (child = self->col_head; child; child = child->next)
				PyInterruptQueue_Add(&queue, child->interrupt_point);
			run_queue = 1;
		}
	} else {
		_pop_child(self, self->col_mainthread);
		//CollateChild_Delete(self->col_mainthread);
		//self->col_mainthread = NULL;
		delete_child = 1;
	}

	PyThread_lock_release(self->col_lock);
	/* We release the GIL *and* collate's lock */

	if (run_queue) {
		PyState_Resume();
		PyInterruptQueue_Finish(&queue);
		PyState_Suspend();
	}

	/* Wait until nothreads is 1 (true, there are no threads)
	 * Sets it to 0 as a side effect */
	PyThread_sem_wait(self->col_nothreads);

	/* We reacquire collate's lock but NOT the GIL */
	PyThread_lock_acquire(self->col_lock);

	assert(self->col_threadcount == 0);
	assert(self->col_state == COLLATE_DYING);
	self->col_state = COLLATE_DEAD;

	PyThread_lock_release(self->col_lock);
	PyState_Resume();
	/* End unlocked region */

	/* Now that we're dead it's safe to check our variables without
	 * acquiring the lock */

	if (delete_child) {
		CollateChild_Delete(self->col_mainthread);
		self->col_mainthread = NULL;
	}

	PyInterrupt_Pop(self->col_baseinterrupt);
	Py_CLEAR(self->col_baseinterrupt);

	if (self->col_failurecount && self->col_resultcount) {
		/* Purge the results so they're not mixed with the failures */
		PyCollateChild *next = self->col_head;
		while (next) {
			PyCollateChild *child = next;
			next = child->next;

			if (child->result != NULL) {
				_pop_child(self, child);
				CollateChild_DeleteWithResult(child);
				self->col_resultcount--;
			}
		}
		assert(self->col_resultcount == 0);
	}

	if (self->col_failurecount) {
		Collate_raisefailure(self);
		return NULL;
	} else {
		Py_INCREF(Py_None);
		return Py_None;
	}
}

static void
collate_baseinterrupt(struct _PyInterruptQueue *queue, void *arg)
{
	//PyCollateObject *self = (PyCollateObject *)arg;

	printf("Mooooo\n");
}

static PyObject *
Collate_add(PyCollateObject *self, PyObject *args, PyObject *kwds)
{
	if (!collate_add_common(self, args, kwds, "collate.add", 0))
		return NULL;

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
Collate_addresult(PyCollateObject *self, PyObject *args, PyObject *kwds)
{
	if (!collate_add_common(self, args, kwds, "collate.addresult", 1))
		return NULL;

	Py_INCREF(Py_None);
	return Py_None;
}

static int
collate_add_common(PyCollateObject *self, PyObject *args, PyObject *kwds,
	char *name, int saveresult)
{
	PyObject *func;
	PyObject *smallargs;

	if (PyTuple_Size(args) < 1) {
		PyErr_Format(PyExc_TypeError,
			"%s() needs a function to be called", name);
		return 0;
	}

	func = PyTuple_GetItem(args, 0);

	if (!PyObject_IsShareable(func)) {
		PyErr_Format(PyExc_TypeError,
			"%s()'s function argument must be shareable, '%s' "
			"object is not", name, func->ob_type->tp_name);
		return 0;
	}

	smallargs = PyTuple_GetSlice(args, 1, PyTuple_Size(args));
	if (smallargs == NULL) {
		return 0;
	}

	if (!PyArg_RequireShareable(name, smallargs, kwds)) {
		Py_DECREF(smallargs);
		return 0;
	}

	if (!collate_spawn_thread(self, func, smallargs, kwds, name, saveresult)) {
		Py_DECREF(smallargs);
		return 0;
	}

	Py_DECREF(smallargs);
	return 1;
}

static int
collate_spawn_thread(PyCollateObject *self, PyObject *func, PyObject *args,
	PyObject *kwds, char *name, int save_result)
{
	PyCollateChild *child;
	PyObject *exc;
	const char *format;

	child = CollateChild_New(self, func, args, kwds);
	if (child == NULL)
		return 0;
	child->save_result = save_result;

	child->tstate = _PyThreadState_New();
	if (child->tstate == NULL) {
		CollateChild_Delete(child);
		PyErr_NoMemory();
		return 0;
	}

	if (self->col_interrupting)
		/* XXX FIXME this is a hack! */
		child->interrupt_point->interrupted = 1;

	/* Begin unlocked region */
	PyState_Suspend();
	PyThread_lock_acquire(self->col_lock);

	if (self->col_state != COLLATE_ALIVE) {
		exc = PyExc_TypeError;
		format = "%s() called in wrong state";
		goto failed;
	}

	if (PyThreadState_Get()->import_depth)
		Py_FatalError("importing is not thread-safe");

	_push_child(self, child);

	if (PyThread_start_new_thread(collate_threadbootstrap, child) == -1) {
		exc = PyExc_RuntimeError;
		format = "%s can't spawn new thread";
		goto failed;
	}

	if (self->col_threadcount == 0) {
		/* Set nothreads to 0 (false, there is a thread) */
		PyThread_sem_wait(self->col_nothreads);
	}
	self->col_threadcount++;

	PyThread_lock_release(self->col_lock);
	PyState_Resume();
	/* End unlocked region */
	return 1;

failed:
	if (self->col_tail == child)
		_pop_child(self, child);
	PyThread_lock_release(self->col_lock);
	PyState_Resume();
	/* End unlocked region */

	if (child->tstate)
		_PyThreadState_Delete(child->tstate);
	CollateChild_Delete(child);

	if (exc != NULL)
		PyErr_Format(exc, format, name);
	else
		PyErr_NoMemory();

	return 0;
}

static void
collate_threadbootstrap(void *arg)
{
	PyInterruptQueue queue;
	int run_queue = 0;
	PyCollateChild *child = (PyCollateChild *)arg;
	PyState_EnterTag entertag;
	PyCollateObject *collate = child->collate;
	int delete_child = 0;

	entertag = _PyState_EnterPreallocated(child->tstate);
	if (!entertag) {
		/* Because we preallocate everything, it should be
		 * impossible to fail. */
		Py_FatalError("PyState_EnterPreallocated failed");
	}

	PyInterrupt_Push(child->interrupt_point);

	child->result = PyObject_Call(child->func, child->args, child->kwds);
	if (!PyArg_RequireShareableReturn("collate._threadbootstrap",
			child->func, child->result))
		Py_CLEAR(child->result);

	PyInterrupt_Pop(child->interrupt_point);

	Py_CLEAR(child->func);
	Py_CLEAR(child->args);
	Py_CLEAR(child->kwds);

	if (child->result != NULL) {
		if (!child->save_result)
			Py_DECREF(child->result);
	} else
		PyErr_Fetch(&child->failure.e_type, &child->failure.e_value,
			&child->failure.e_traceback);

	/* Begin unlocked region */
	PyState_Suspend();
	PyThread_lock_acquire(collate->col_lock);

	if (child->result != NULL) {
		if (child->save_result)
			collate->col_resultcount++;
		else {
			/* XXX child->result was DECREF's earlier */
			child->result = NULL;
			_pop_child(collate, child);
			//CollateChild_Delete(child);
			delete_child = 1;
		}
	} else {
		collate->col_failurecount++;
		if (collate->col_failurecount == 1) {
			PyCollateChild *otherchild;

			PyInterruptQueue_Init(&queue);
			for (otherchild = collate->col_head; otherchild;
					otherchild = otherchild->next)
				PyInterruptQueue_Add(&queue, otherchild->interrupt_point);
			run_queue = 1;
		}
	}

	PyThread_lock_release(collate->col_lock);
	PyState_Resume();
	/* End unlocked region */

	if (delete_child)
		CollateChild_Delete(child);

	if (run_queue)
		PyInterruptQueue_Finish(&queue);

	PyState_Exit(entertag);

	/* This part is evil.  We've already released all our access to
	 * the interpreter, but we're going to access collate's lock,
	 * threadcount, and semaphore anyway.  This should work so long
	 * as there's a main thread with its own refcount blocked on the
	 * semaphore/lock.  It also assumes that the unlock function
	 * stops touching the lock's memory as soon as it allows the
	 * main thread to run. */
	PyThread_lock_acquire(collate->col_lock);

	collate->col_threadcount--;
	if (collate->col_threadcount == 0) {
		/* Set nothreads to 1 (true, there are no threads) */
		PyThread_sem_post(collate->col_nothreads);
	}

	PyThread_lock_release(collate->col_lock);

	PyThread_exit_thread();
}

static void
_push_child(PyCollateObject *self, PyCollateChild *child)
{
	child->next = NULL;
	child->prev = self->col_tail;
	if (self->col_tail == NULL) {
		self->col_head = child;
		self->col_tail = child;
	} else {
		self->col_tail->next = child;
		self->col_tail = child;
	}
}

static void
_pop_child(PyCollateObject *self, PyCollateChild *child)
{
	if (child->prev != NULL)
		child->prev->next = child->next;
	if (child->next != NULL)
		child->next->prev = child->prev;
	if (self->col_tail == child)
		self->col_tail = child->prev;
	if (self->col_head == child)
		self->col_head = child->next;

	child->prev = NULL;
	child->next = NULL;
}

static PyObject *
Collate_getresults(PyCollateObject *self)
{
	int state;
	PyObject *results;
	Py_ssize_t i;

	/* Begin unlocked region */
	PyState_Suspend();
	PyThread_lock_acquire(self->col_lock);

	state = self->col_state;

	PyThread_lock_release(self->col_lock);
	PyState_Resume();
	/* End unlocked region */

	if (state != COLLATE_DEAD) {
		PyErr_SetString(PyExc_TypeError, "collate.getresults() "
			"called in wrong state");
		return NULL;
	}

	/* Once we know the state is COLLATE_DEAD we can be sure no
	 * other threads will access us.  Thus, we can rely on the GIL. */

	assert(!self->col_failurecount);

	results = PyList_New(self->col_resultcount);
	if (results == NULL)
		return NULL;

	i = 0;
	while (self->col_head) {
		assert(i < self->col_resultcount);
		PyCollateChild *child = self->col_head;

		_pop_child(self, child);
		assert(child->failure.e_type == NULL);
		assert(child->result != NULL);

		/* Copy across, stealing references */
		PyList_SET_ITEM(results, i, child->result);
		child->result = NULL;
		CollateChild_Delete(child);
		i++;
	}
	assert(i == self->col_resultcount);
	self->col_resultcount = 0;
	return results;
}

static void
Collate_raisefailure(PyCollateObject *self)
{
	Py_ssize_t i;
	PyObject *failures;

	assert(self->col_state == COLLATE_DEAD);
	assert(self->col_resultcount == 0);
	assert(self->col_failurecount);

	if (self->col_failurecount == 1) {
		PyCollateChild *child = self->col_head;

		_pop_child(self, child);
		/* Steals the references */
		//printf("snoop: %d\n", Py_RefcntSnoop(child->failure.e_type));
		//printf("snoop: %d\n", Py_RefcntSnoop(child->failure.e_value));
		//printf("snoop: %d\n", Py_RefcntSnoop(child->failure.e_traceback));
		PyErr_Restore(child->failure.e_type,
			child->failure.e_value,
			child->failure.e_traceback);
		//printf("snoop2: %d\n", Py_RefcntSnoop(child->failure.e_type));
		//printf("snoop2: %d\n", Py_RefcntSnoop(child->failure.e_value));
		//printf("snoop2: %d\n", Py_RefcntSnoop(child->failure.e_traceback));
		child->failure.e_type = NULL;
		child->failure.e_value = NULL;
		child->failure.e_traceback = NULL;

		CollateChild_Delete(child);
		self->col_failurecount = 0;

		assert(self->col_head == NULL);

		return;
	}

	failures = PyList_New(self->col_failurecount);
	if (failures == NULL) {
		PyErr_NoMemory();
		goto failed;
	}

	i = 0;
	while (self->col_head) {
		PyCollateChild *child = self->col_head;
		PyObject *tup;

		assert(i < self->col_failurecount);
		_pop_child(self, child);

		assert(child->result == NULL);
		assert(child->failure.e_type != NULL);
		assert(child->failure.e_value != NULL);
		assert(child->failure.e_traceback != NULL);
		/* XXX FIXME temporary hack */
		Py_DECREF(child->failure.e_traceback);
		child->failure.e_traceback = Py_None;
		Py_INCREF(Py_None);

		/* Creates new references */
		tup = Py_BuildValue("OOO", child->failure.e_type,
			child->failure.e_value, child->failure.e_traceback);
		if (tup == NULL) {
			Py_DECREF(failures);
			failures = NULL;
			goto failed;
		}

		PyList_SET_ITEM(failures, i, tup);
		CollateChild_DeleteWithFailure(child);
		i++;
	}

	PyErr_SetObject(PyExc_MultipleError, failures);
	Py_DECREF(failures);

	assert(i == self->col_failurecount);
	self->col_failurecount = 0;
	return;

failed:
	while (self->col_head) {
		PyCollateChild *child = self->col_head;
		_pop_child(self, child);
		CollateChild_DeleteWithFailure(child);
	}
	self->col_failurecount = 0;
}

PyDoc_STRVAR(Collate___enter____doc__, "");
PyDoc_STRVAR(Collate___exit____doc__, "");
PyDoc_STRVAR(Collate_add__doc__, "add(func, *args, **kwargs) -> None");
PyDoc_STRVAR(Collate_addresult__doc__, "addresult(func, *args, **kwargs) -> None");
PyDoc_STRVAR(Collate_getresults__doc__, "getresults() -> list");

static PyMethodDef Collate_methods[] = {
	{"__enter__",	(PyCFunction)Collate___enter__,	METH_NOARGS,
		Collate___enter____doc__},
	{"__exit__",	(PyCFunction)Collate___exit__,	METH_VARARGS,
		Collate___exit____doc__},
	{"add",		(PyCFunction)Collate_add,	METH_VARARGS | METH_KEYWORDS,
		Collate_add__doc__},
	{"addresult",	(PyCFunction)Collate_addresult,	METH_VARARGS | METH_KEYWORDS,
		Collate_addresult__doc__},
	{"getresults",	(PyCFunction)Collate_getresults,	METH_NOARGS,
		Collate_getresults__doc__},
	{NULL,		NULL}		/* sentinel */
};

PyTypeObject PyCollate_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"_threadtoolsmodule.collate",	/*tp_name*/
	sizeof(PyCollateObject),	/*tp_basicsize*/
	0,			/*tp_itemsize*/
	(destructor)Collate_dealloc,	/*tp_dealloc*/
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
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_SHAREABLE,	/*tp_flags*/
	0,			/*tp_doc*/
	0,			/*tp_traverse*/
	0,			/*tp_clear*/
	0,			/*tp_richcompare*/
	0,			/*tp_weaklistoffset*/
	0,			/*tp_iter*/
	0,			/*tp_iternext*/
	Collate_methods,	/*tp_methods*/
	0,			/*tp_members*/
	0,			/*tp_getset*/
	0,			/*tp_base*/
	0,			/*tp_dict*/
	0,			/*tp_descr_get*/
	0,			/*tp_descr_set*/
	0,			/*tp_dictoffset*/
	0,			/*tp_init*/
	Collate_new,		/*tp_new*/
};

