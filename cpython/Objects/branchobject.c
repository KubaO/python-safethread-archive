
#include "Python.h"
#include "ceval.h"
#include "interruptobject.h"
#include "branchobject.h"


/* Branch methods */

static void branch_baseinterrupt(struct _PyInterruptQueue *queue, void *arg);
static void branchchild_interrupt(PyInterruptQueue *queue, void *arg);
static int branch_add_common(PyBranchObject *self, PyObject *args,
    PyObject *kwds, char *name, int saveresult);
static void branch_threadbootstrap(void *arg);
static int branch_spawn_thread(PyBranchObject *self, PyObject *func,
    PyObject *args, PyObject *kwds, char *name, int save_result);

static void BranchChild_Delete(PyBranchChild *child);
static void BranchChild_DeleteWithResult(PyBranchChild *child);
static void BranchChild_DeleteWithException(PyBranchChild *child);
static void _push_child(PyBranchObject *self, PyBranchChild *child);
static void _pop_child(PyBranchObject *self, PyBranchChild *child);

static PyObject *Branch_getresults(PyBranchObject *self);
static void Branch_raiseexception(PyBranchObject *self);

static PyObject *
Branch_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyBranchObject *self;

    assert(type != NULL);

    self = PyObject_NEW(PyBranchObject, type);
    if (self == NULL)
        return NULL;

    self->col_lock = PyThread_lock_allocate();
    if (self->col_lock == NULL) {
        PyObject_DEL(self);
        PyErr_SetString(PyExc_RuntimeError, "can't allocate lock");
        return NULL;
    }

    self->col_state = BRANCH_NEW;
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
    self->col_exceptioncount = 0;

    return (PyObject *)self;
}

static void
Branch_dealloc(PyBranchObject *self)
{
    if (self->col_state != BRANCH_NEW && self->col_state != BRANCH_DEAD)
        Py_FatalError("Invalid state in Branch_dealloc()");
    if (self->col_threadcount != 0)
        Py_FatalError("Remaining threads in Branch_dealloc()");

    PyThread_lock_free(self->col_lock);
    PyThread_sem_free(self->col_nothreads);

    assert(self->col_baseinterrupt == NULL);

    while (self->col_head) {
        PyBranchChild *child = self->col_head;
        _pop_child(self, child);
        BranchChild_DeleteWithResult(child);
        self->col_resultcount--;
    }

    assert(self->col_resultcount == 0);
    assert(self->col_exceptioncount == 0);

    PyObject_DEL(self);
}

static PyBranchChild *
BranchChild_New(PyBranchObject *branch, PyObject *func, PyObject *args,
        PyObject *kwds)
{
    PyBranchChild *child;

    child = malloc(sizeof(PyBranchChild));
    if (child == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    child->interp = PyThreadState_Get()->interp;
    child->tstate = NULL;
    child->interrupt_point = PyInterrupt_New(branchchild_interrupt,
            NULL, NULL);
    if (child->interrupt_point == NULL) {
        free(child);
        PyErr_NoMemory();
        return NULL;
    }
    child->branch = branch;

    Py_INCREF(func);
    child->func = func;
    Py_INCREF(args);
    child->args = args;
    Py_XINCREF(kwds);
    child->kwds = kwds;

    child->save_result = 0;
    child->result = NULL;
    child->exception = NULL;
    child->prev = NULL;
    child->next = NULL;

    return child;
}

static void
BranchChild_Delete(PyBranchChild *child)
{
    Py_DECREF(child->interrupt_point);

    assert(child->prev == NULL);
    assert(child->next == NULL);

    assert(child->result == NULL);
    assert(child->exception == NULL);

    Py_XDECREF(child->func);
    Py_XDECREF(child->args);
    Py_XDECREF(child->kwds);

    free(child);
}

static void
BranchChild_DeleteWithResult(PyBranchChild *child)
{
    Py_DECREF(child->interrupt_point);

    assert(child->prev == NULL);
    assert(child->next == NULL);

    Py_XDECREF(child->result);
    assert(child->exception == NULL);

    Py_XDECREF(child->func);
    Py_XDECREF(child->args);
    Py_XDECREF(child->kwds);

    free(child);
}

static void
BranchChild_DeleteWithException(PyBranchChild *child)
{
    Py_DECREF(child->interrupt_point);

    assert(child->prev == NULL);
    assert(child->next == NULL);

    assert(child->result == NULL);
    Py_XDECREF(child->exception);

    Py_XDECREF(child->func);
    Py_XDECREF(child->args);
    Py_XDECREF(child->kwds);

    free(child);
}

static void
branchchild_interrupt(PyInterruptQueue *queue, void *arg)
{
    Py_FatalError("branchchild_interrupt called");
    /* XXX FIXME */
}

static PyObject *
Branch___enter__(PyBranchObject *self)
{
    PyInterruptObject *baseinterrupt;
    PyBranchChild *mainchild = BranchChild_New(self, Py_None, Py_None, Py_None);
    if (mainchild == NULL)
        return NULL;

    baseinterrupt = PyInterrupt_New(branch_baseinterrupt, self, NULL);
    if (baseinterrupt == NULL) {
        BranchChild_Delete(mainchild);
        return NULL;
    }

    /* Begin unlocked region */
    PyState_Suspend();
    PyThread_lock_acquire(self->col_lock);

    if (self->col_state != BRANCH_NEW) {
        PyThread_lock_release(self->col_lock);
        PyState_Resume();
        /* End unlocked region */

        Py_DECREF(baseinterrupt);
        BranchChild_Delete(mainchild);
        PyErr_SetString(PyExc_TypeError, "branch.__enter__() called in "
            "wrong state");
        return NULL;
    }

    self->col_mainthread = mainchild;
    _push_child(self, mainchild);
    /* XXX setup interrupt stack for current thread */
    self->col_baseinterrupt = baseinterrupt;
    PyInterrupt_Push(self->col_baseinterrupt);
    PyInterrupt_Push(self->col_mainthread->interrupt_point);

    self->col_state = BRANCH_ALIVE;

    PyThread_lock_release(self->col_lock);
    PyState_Resume();
    /* End unlocked region */

    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *
Branch___exit__(PyBranchObject *self, PyObject *args)
{
    PyInterruptQueue queue;
    int run_queue = 0;
    PyObject *type, *val, *tb;
    int delete_child = 0;

    if (!PyArg_ParseTuple(args, "OOO", &type, &val, &tb))
        Py_FatalError("Branch.__exit__() got bad arguments");

    if (type == Py_None)
        val = NULL;
    else {
        Py_INCREF(type);
        Py_INCREF(val);
        Py_INCREF(tb);
        val = PyErr_SimplifyException(type, val, tb);
    }

    /* Begin unlocked region */
    PyState_Suspend();
    PyThread_lock_acquire(self->col_lock);

    assert(self->col_state == BRANCH_ALIVE);
    self->col_state = BRANCH_DYING;

    /* XXX pop interrupt stack for current thread */
    if (val != NULL) {
        self->col_exceptioncount++;
        self->col_mainthread->exception = val;
        if (self->col_exceptioncount == 1) {
            PyBranchChild *child;

            PyInterruptQueue_Init(&queue);
            for (child = self->col_head; child; child = child->next)
                PyInterruptQueue_Add(&queue, child->interrupt_point);
            run_queue = 1;
        }
    } else
        delete_child = 1;

    PyThread_lock_release(self->col_lock);
    /* We release the GIL *and* branch's lock */

    if (run_queue) {
        PyState_Resume();
        PyInterruptQueue_Finish(&queue);
        PyState_Suspend();
    }

    /* Wait until nothreads is 1 (true, there are no threads)
     * Sets it to 0 as a side effect */
    PyThread_sem_wait(self->col_nothreads);

    /* We reacquire branch's lock but NOT the GIL */
    PyThread_lock_acquire(self->col_lock);

    assert(self->col_threadcount == 0);
    assert(self->col_state == BRANCH_DYING);
    self->col_state = BRANCH_DEAD;

    PyThread_lock_release(self->col_lock);
    PyState_Resume();
    /* End unlocked region */

    /* Now that we're dead it's safe to check our variables without
     * acquiring the lock */

    PyInterrupt_Pop(self->col_mainthread->interrupt_point);
    PyInterrupt_Pop(self->col_baseinterrupt);
    Py_CLEAR(self->col_baseinterrupt);

    if (delete_child) {
        _pop_child(self, self->col_mainthread);
        BranchChild_Delete(self->col_mainthread);
        self->col_mainthread = NULL;
    }

    if (self->col_exceptioncount && self->col_resultcount) {
        /* Purge the results so they're not mixed with the exceptions */
        PyBranchChild *next = self->col_head;
        while (next) {
            PyBranchChild *child = next;
            next = child->next;

            if (child->result != NULL) {
                _pop_child(self, child);
                BranchChild_DeleteWithResult(child);
                self->col_resultcount--;
            }
        }
        assert(self->col_resultcount == 0);
    }

    if (self->col_exceptioncount) {
        Branch_raiseexception(self);
        return NULL;
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}

static void
branch_baseinterrupt(struct _PyInterruptQueue *queue, void *arg)
{
    PyBranchChild *child;
    PyBranchObject *self = (PyBranchObject *)arg;

    PyThread_lock_acquire(self->col_lock);
    self->col_interrupting = 1;
    for (child = self->col_head; child; child = child->next) {
        PyInterruptQueue_Add(queue, child->interrupt_point);
    }
    PyThread_lock_release(self->col_lock);
}

static PyObject *
Branch_add(PyBranchObject *self, PyObject *args, PyObject *kwds)
{
    if (!branch_add_common(self, args, kwds, "branch.add", 0))
        return NULL;

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
Branch_addresult(PyBranchObject *self, PyObject *args, PyObject *kwds)
{
    if (!branch_add_common(self, args, kwds, "branch.addresult", 1))
        return NULL;

    Py_INCREF(Py_None);
    return Py_None;
}

static int
branch_add_common(PyBranchObject *self, PyObject *args, PyObject *kwds,
        char *name, int saveresult)
{
    PyObject *func;
    PyObject *smallargs;

    if (PyTuple_Size(args) < 1) {
        PyErr_Format(PyExc_TypeError, "%s() needs a function to be "
            "called", name);
        return 0;
    }

    func = PyTuple_GetItem(args, 0);

    if (!PyObject_IsShareable(func)) {
        PyErr_Format(PyExc_TypeError, "%s()'s function argument must be "
            "shareable, '%s' object is not", name, func->ob_type->tp_name);
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

    if (!branch_spawn_thread(self, func, smallargs, kwds, name, saveresult)) {
        Py_DECREF(smallargs);
        return 0;
    }

    Py_DECREF(smallargs);
    return 1;
}

static int
branch_spawn_thread(PyBranchObject *self, PyObject *func, PyObject *args,
        PyObject *kwds, char *name, int save_result)
{
    PyBranchChild *child;
    PyObject *exc;
    const char *format;

    child = BranchChild_New(self, func, args, kwds);
    if (child == NULL)
        return 0;
    child->save_result = save_result;

    child->tstate = _PyThreadState_New();
    if (child->tstate == NULL) {
        BranchChild_Delete(child);
        PyErr_NoMemory();
        return 0;
    }

    if (self->col_interrupting)
        /* XXX FIXME this is a hack! */
        child->interrupt_point->interrupted = 1;

    /* Begin unlocked region */
    PyState_Suspend();
    PyThread_lock_acquire(self->col_lock);

    if (self->col_state != BRANCH_ALIVE) {
        exc = PyExc_TypeError;
        format = "%s() called in wrong state";
        goto failed;
    }

    if (PyThreadState_Get()->import_depth)
        Py_FatalError("importing is not thread-safe");

    _push_child(self, child);

    if (PyThread_start_new_thread(NULL, branch_threadbootstrap, child) < 0) {
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
    BranchChild_Delete(child);

    if (exc != NULL)
        PyErr_Format(exc, format, name);
    else
        PyErr_NoMemory();

    return 0;
}

static void
branch_threadbootstrap(void *arg)
{
    PyInterruptQueue queue;
    int run_queue = 0;
    PyBranchChild *child = (PyBranchChild *)arg;
    PyState_EnterTag entertag;
    PyBranchObject *branch = child->branch;
    int delete_child = 0;

    entertag = _PyState_EnterPreallocated(child->tstate);
    if (!entertag) {
        /* Because we preallocate everything, it should be
         * impossible to fail. */
        Py_FatalError("PyState_EnterPreallocated failed");
    }

    PyInterrupt_Push(child->interrupt_point);

    child->result = PyObject_Call(child->func, child->args, child->kwds);
    if (!PyArg_RequireShareableReturn("branch._threadbootstrap",
            child->func, child->result))
        Py_CLEAR(child->result);

    PyInterrupt_Pop(child->interrupt_point);

    Py_CLEAR(child->func);
    Py_CLEAR(child->args);
    Py_CLEAR(child->kwds);

    if (child->result != NULL) {
        if (!child->save_result)
            Py_DECREF(child->result);
    } else {
        PyObject *type, *val, *tb;
        PyErr_Fetch(&type, &val, &tb);
        child->exception = PyErr_SimplifyException(type, val, tb);
    }

    /* Begin unlocked region */
    PyState_Suspend();
    PyThread_lock_acquire(branch->col_lock);

    if (child->result != NULL) {
        if (child->save_result)
            branch->col_resultcount++;
        else {
            /* XXX child->result was DECREF's earlier */
            child->result = NULL;
            _pop_child(branch, child);
            //BranchChild_Delete(child);
            delete_child = 1;
        }
    } else {
        branch->col_exceptioncount++;
        if (branch->col_exceptioncount == 1) {
            PyBranchChild *otherchild;

            PyInterruptQueue_Init(&queue);
            for (otherchild = branch->col_head; otherchild;
                    otherchild = otherchild->next)
                PyInterruptQueue_Add(&queue, otherchild->interrupt_point);
            run_queue = 1;
        }
    }

    PyThread_lock_release(branch->col_lock);
    PyState_Resume();
    /* End unlocked region */

    if (delete_child)
        BranchChild_Delete(child);

    if (run_queue)
        PyInterruptQueue_Finish(&queue);

    PyState_Exit(entertag);

    /* This part is evil.  We've already released all our access to
     * the interpreter, but we're going to access branch's lock,
     * threadcount, and semaphore anyway.  This should work so long
     * as there's a main thread with its own refcount blocked on the
     * semaphore/lock.  It also assumes that the unlock function
     * stops touching the lock's memory as soon as it allows the
     * main thread to run. */
    PyThread_lock_acquire(branch->col_lock);

    branch->col_threadcount--;
    if (branch->col_threadcount == 0) {
        /* Set nothreads to 1 (true, there are no threads) */
        PyThread_sem_post(branch->col_nothreads);
    }

    PyThread_lock_release(branch->col_lock);
}

static void
_push_child(PyBranchObject *self, PyBranchChild *child)
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
_pop_child(PyBranchObject *self, PyBranchChild *child)
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
Branch_getresults(PyBranchObject *self)
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

    if (state != BRANCH_DEAD) {
        PyErr_SetString(PyExc_TypeError, "branch.getresults() called in "
            "wrong state");
        return NULL;
    }

    /* Once we know the state is BRANCH_DEAD we can be sure no
     * other threads will access us.  Thus, we can rely on the GIL. */

    assert(!self->col_exceptioncount);

    results = PyList_New(self->col_resultcount);
    if (results == NULL)
        return NULL;

    i = 0;
    while (self->col_head) {
        assert(i < self->col_resultcount);
        PyBranchChild *child = self->col_head;

        _pop_child(self, child);
        assert(child->exception == NULL);
        assert(child->result != NULL);

        /* Copy across, stealing references */
        PyList_SET_ITEM(results, i, child->result);
        child->result = NULL;
        BranchChild_Delete(child);
        i++;
    }
    assert(i == self->col_resultcount);
    self->col_resultcount = 0;
    return results;
}

static void
Branch_raiseexception(PyBranchObject *self)
{
    Py_ssize_t i;
    PyObject *causes;
    PyObject *interesting = NULL;
    int multiple = 0;
    PyObject *type, *val, *tb;

    assert(self->col_state == BRANCH_DEAD);
    assert(self->col_resultcount == 0);
    assert(self->col_exceptioncount);

    causes = PyTuple_New(self->col_exceptioncount);
    if (causes == NULL) {
        PyErr_NoMemory();
        goto failed;
    }

    i = 0;
    while (self->col_head) {
        PyBranchChild *child = self->col_head;
        PyObject *tup;

        assert(i < self->col_exceptioncount);
        _pop_child(self, child);

        assert(child->result == NULL);
        assert(child->exception != NULL);

        if (!PyErr_GivenExceptionMatches(child->exception,
                PyExc_Interrupted)) {
            if (interesting != NULL) {
                Py_DECREF(interesting);
                interesting = NULL;
                multiple = 1;
            } else if (!multiple) {
                Py_INCREF(child->exception);
                interesting = child->exception;
            }
        }

        Py_INCREF(child->exception);
        PyTuple_SET_ITEM(causes, i, child->exception);
        BranchChild_DeleteWithException(child);
        i++;
    }

    if (interesting) {
        PyErr_SetObject((PyObject *)Py_Type(interesting),
            ((PyBaseExceptionObject *)interesting)->args);
        Py_DECREF(interesting);
    } else if (self->col_interrupting && !multiple)
        PyErr_SetNone(PyExc_Interrupted);
    else
        PyErr_SetNone(PyExc_MultipleError);

    PyErr_Fetch(&type, &val, &tb);
    PyErr_NormalizeException(&type, &val, &tb);
    PyException_SetCause(val, causes); /* Consumes a reference to causes */
    PyErr_Restore(type, val, tb);

    assert(i == self->col_exceptioncount);
    self->col_exceptioncount = 0;
    return;

failed:
    while (self->col_head) {
        PyBranchChild *child = self->col_head;
        _pop_child(self, child);
        BranchChild_DeleteWithException(child);
    }
    self->col_exceptioncount = 0;
}

PyDoc_STRVAR(Branch___enter____doc__, "");
PyDoc_STRVAR(Branch___exit____doc__, "");
PyDoc_STRVAR(Branch_add__doc__, "add(func, *args, **kwargs) -> None");
PyDoc_STRVAR(Branch_addresult__doc__, "addresult(func, *args, **kwargs) -> None");
PyDoc_STRVAR(Branch_getresults__doc__, "getresults() -> list");

static PyMethodDef Branch_methods[] = {
    {"__enter__",       (PyCFunction)Branch___enter__,  METH_NOARGS,
        Branch___enter____doc__},
    {"__exit__",        (PyCFunction)Branch___exit__,   METH_VARARGS,
        Branch___exit____doc__},
    {"add",             (PyCFunction)Branch_add,        METH_VARARGS | METH_KEYWORDS,
        Branch_add__doc__},
    {"addresult",       (PyCFunction)Branch_addresult,  METH_VARARGS | METH_KEYWORDS,
        Branch_addresult__doc__},
    {"getresults",      (PyCFunction)Branch_getresults, METH_NOARGS,
        Branch_getresults__doc__},
    {NULL,              NULL}  /* sentinel */
};

PyTypeObject PyBranch_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "_threadtoolsmodule.branch",        /*tp_name*/
    sizeof(PyBranchObject),             /*tp_basicsize*/
    0,                                  /*tp_itemsize*/
    (destructor)Branch_dealloc,         /*tp_dealloc*/
    0,                                  /*tp_print*/
    0,                                  /*tp_getattr*/
    0,                                  /*tp_setattr*/
    0,                                  /*tp_compare*/
    0,                                  /*tp_repr*/
    0,                                  /*tp_as_number*/
    0,                                  /*tp_as_sequence*/
    0,                                  /*tp_as_mapping*/
    0,                                  /*tp_hash*/
    0,                                  /*tp_call*/
    0,                                  /*tp_str*/
    PyObject_GenericGetAttr,            /*tp_getattro*/
    0,                                  /*tp_setattro*/
    0,                                  /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_SHAREABLE,  /*tp_flags*/
    0,                                  /*tp_doc*/
    0,                                  /*tp_traverse*/
    0,                                  /*tp_clear*/
    0,                                  /*tp_richcompare*/
    0,                                  /*tp_weaklistoffset*/
    0,                                  /*tp_iter*/
    0,                                  /*tp_iternext*/
    Branch_methods,                     /*tp_methods*/
    0,                                  /*tp_members*/
    0,                                  /*tp_getset*/
    0,                                  /*tp_base*/
    0,                                  /*tp_dict*/
    0,                                  /*tp_descr_get*/
    0,                                  /*tp_descr_set*/
    0,                                  /*tp_dictoffset*/
    0,                                  /*tp_init*/
    Branch_new,                         /*tp_new*/
};

