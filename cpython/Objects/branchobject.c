
#include "Python.h"
#include "ceval.h"
#include "cancelobject.h"
#include "monitorobject.h"
#include "branchobject.h"


/* Branch methods */

static void branch_basecancel(PyCancelQueue *queue, void *arg);
static void branchchild_cancel(PyCancelQueue *queue, void *arg);
static int branch_add_common(PyBranchObject *self, PyObject *args,
    PyObject *kwds, char *name, int saveresult);
static void branch_threadbootstrap(void *arg);
static int branch_spawn_thread(PyBranchObject *self, PyObject *func,
    PyObject *args, PyObject *kwds, char *name, int save_result);

static void BranchChild_Delete(PyBranchChild *child);

static void branch_cleanchildren(PyBranchObject *self);
static PyObject *Branch_getresults(PyBranchObject *self);
static void Branch_raiseexception(PyBranchObject *self);

static PyObject *
Branch_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyBranchObject *self;

    assert(type != NULL);

    self = PyObject_New(type);
    if (self == NULL)
        return NULL;

    self->crit = PyCritical_Allocate(PyCRITICAL_NORMAL);
    if (self->crit == NULL) {
#warning Branch_new should not call PyObject_Del
        PyObject_Del(self);
        PyErr_NoMemory();
        return NULL;
    }

    self->col_state = BRANCH_NEW;
    self->col_ownerthread = NULL;
    self->col_threads = NULL;
    PyLinkedList_InitBase(&self->children,
        offsetof(PyBranchChild, children_links));
    PyLinkedList_InitBase(&self->alive,
        offsetof(PyBranchChild, alive_links));
    PyLinkedList_InitBase(&self->deletable,
        offsetof(PyBranchChild, alive_links));

    self->col_basecancel = NULL;

    self->col_cancelling = 0;
    self->col_resultcount = 0;
    self->col_exceptioncount = 0;

    return (PyObject *)self;
}

static void
Branch_dealloc(PyBranchObject *self)
{
    PyBranchChild *child = NULL;

    if (self->col_state != BRANCH_NEW && self->col_state != BRANCH_DEAD)
        Py_FatalError("Invalid state in Branch_dealloc()");

    PyCritical_Free(self->crit);

    assert(self->col_basecancel == NULL);

    assert(PyLinkedList_Empty(&self->alive));

    while (PyLinkedList_Next(&self->children, &child)) {
        Py_CLEAR(child->result);
        PyLinkedList_Append(&self->deletable, child);
        self->col_resultcount--;
    }

    branch_cleanchildren(self);

    assert(self->col_resultcount == 0);
    assert(self->col_exceptioncount == 0);
    assert(PyLinkedList_Empty(&self->children));
    assert(PyLinkedList_Empty(&self->deletable));

    PyObject_Del(self);
}

static PyBranchChild *
BranchChild_New(PyBranchObject *branch, PyObject *func, PyObject *args,
        PyObject *kwds, int new_pystate)
{
    PyBranchChild *child;

    child = malloc(sizeof(PyBranchChild));
    if (child == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    if (new_pystate) {
        child->pystate = _PyState_New();
        if (child->pystate == NULL) {
            free(child);
            PyErr_NoMemory();
            return NULL;
        }
    } else
        child->pystate = PyState_Get();

    child->cancel_scope = PyCancel_New(branchchild_cancel, NULL, child->pystate);
    if (child->cancel_scope == NULL) {
        if (new_pystate)
            _PyState_Delete(child->pystate);
        free(child);
        PyErr_NoMemory();
        return NULL;
    }
    child->branch = branch;
    child->dead = PyThread_flag_allocate();
    if (child->dead == NULL) {
        Py_DECREF(child->cancel_scope);
        if (new_pystate)
            _PyState_Delete(child->pystate);
        free(child);
        PyErr_NoMemory();
        return NULL;
    }

    child->waitfor.lock = PyThread_lock_allocate();
    if (child->waitfor.lock == NULL) {
        Py_DECREF(child->cancel_scope);
        if (new_pystate)
            _PyState_Delete(child->pystate);
        PyThread_flag_free(child->dead);
        free(child);
        PyErr_NoMemory();
        return NULL;
    }

    Py_INCREF(func);
    child->func = func;
    Py_INCREF(args);
    child->args = args;
    Py_XINCREF(kwds);
    child->kwds = kwds;

    child->save_result = 0;
    child->result = NULL;
    child->exception = NULL;
    PyLinkedList_InitNode(&child->children_links);
    PyLinkedList_InitNode(&child->alive_links);

    child->waitfor.self = child;
    child->waitfor.blocker = NULL;
    child->waitfor.checking_deadlock = 0;
    child->waitfor.abortfunc = NULL;
    PyLinkedList_InitNode(&child->waitfor.inspection_links);

    return child;
}

static void
BranchChild_Delete(PyBranchChild *child)
{
    Py_DECREF(child->cancel_scope);

    assert(PyLinkedList_Detached(&child->children_links));
    assert(PyLinkedList_Detached(&child->alive_links));

    assert(child->result == NULL);
    assert(child->exception == NULL);

    PyThread_flag_free(child->dead);

    Py_XDECREF(child->func);
    Py_XDECREF(child->args);
    Py_XDECREF(child->kwds);

    assert(child->waitfor.blocker == NULL);
    assert(child->waitfor.checking_deadlock == 0);
    assert(PyLinkedList_Detached(&child->waitfor.inspection_links));
    PyThread_lock_free(child->waitfor.lock);

    free(child);
}

static void
branchchild_cancel(PyCancelQueue *queue, void *arg)
{
    Py_FatalError("branchchild_cancel called");
    /* XXX FIXME */
}

static PyObject *
Branch___enter__(PyBranchObject *self)
{
    PyCancelObject *basecancel;
    PyBranchChild *mainchild = BranchChild_New(self, Py_None, Py_None, Py_None, 0);
    if (mainchild == NULL)
        return NULL;

    basecancel = PyCancel_New(branch_basecancel, self, PyState_Get());
    if (basecancel == NULL) {
        BranchChild_Delete(mainchild);
        return NULL;
    }

    PyCritical_Enter(self->crit);

    if (self->col_state != BRANCH_NEW) {
        PyCritical_Exit(self->crit);

        Py_DECREF(basecancel);
        BranchChild_Delete(mainchild);
        PyErr_SetString(PyExc_TypeError, "branch.__enter__() called in "
            "wrong state");
        return NULL;
    }

    self->col_mainthread = mainchild;
    PyLinkedList_Append(&self->children, mainchild);
    PyLinkedList_Append(&self->alive, mainchild);
    /* XXX setup cancel stack for current thread */
    self->col_basecancel = basecancel;
    PyCancel_Push(self->col_basecancel);
    PyCancel_Push(self->col_mainthread->cancel_scope);

    self->col_state = BRANCH_ALIVE;

    PyCritical_Exit(self->crit);

    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *
Branch___exit__(PyBranchObject *self, PyObject *args)
{
    PyCancelQueue queue;
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

    PyCritical_Enter(self->crit);

    assert(self->col_state == BRANCH_ALIVE);
    self->col_state = BRANCH_DYING;

    /* XXX pop cancel stack for current thread */
    if (val != NULL) {
        self->col_exceptioncount++;
        self->col_mainthread->exception = val;
        if (self->col_exceptioncount == 1) {
            PyBranchChild *child = NULL;

            PyCancelQueue_Init(&queue);
            while (PyLinkedList_Next(&self->alive, &child))
                PyCancelQueue_Cancel(&queue, child->cancel_scope);
            run_queue = 1;
        }
    } else
        delete_child = 1;

    PyCritical_Exit(self->crit);

    if (run_queue)
        PyCancelQueue_Finish(&queue);

    PyCritical_Enter(self->crit);
    PyThread_flag_set(self->col_mainthread->dead);

    PyCancel_Pop(self->col_mainthread->cancel_scope);

    PyLinkedList_Remove(&self->col_mainthread->alive_links);
    if (delete_child) {
        PyLinkedList_Append(&self->deletable, self->col_mainthread);
        self->col_mainthread = NULL;
    }

    while (!PyLinkedList_Empty(&self->alive)) {
        PyBranchChild *child = PyLinkedList_First(&self->alive);

        PyCritical_Exit(self->crit);
        _PyMonitorSpace_WaitForBranchChild(child);
        PyCritical_Enter(self->crit);
    }

    assert(self->col_state == BRANCH_DYING);
    self->col_state = BRANCH_DEAD;

    PyCancel_Pop(self->col_basecancel);
    Py_CLEAR(self->col_basecancel);

    branch_cleanchildren(self);

    PyCritical_Exit(self->crit);
    /* Now that we're dead it's safe to check our variables without
     * holding the crit */

    if (self->col_exceptioncount && self->col_resultcount) {
        /* Purge the results so they're not mixed with the exceptions */
        PyBranchChild *child = NULL;

        while (PyLinkedList_Next(&self->children, &child)) {
            if (child->result != NULL) {
                Py_CLEAR(child->result);
                PyLinkedList_Append(&self->deletable, &child);
                self->col_resultcount--;
            }
        }

        assert(self->col_resultcount == 0);
    }

    branch_cleanchildren(self);

    if (self->col_exceptioncount) {
        Branch_raiseexception(self);
        return NULL;
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}

static void
branch_basecancel(PyCancelQueue *queue, void *arg)
{
    PyBranchChild *child = NULL;
    PyBranchObject *self = (PyBranchObject *)arg;

    PyCritical_Enter(self->crit);
    self->col_cancelling = 1;

    while (PyLinkedList_Next(&self->alive, &child))
        PyCancelQueue_Cancel(queue, child->cancel_scope);
    PyCritical_Exit(self->crit);
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

    child = BranchChild_New(self, func, args, kwds, 1);
    if (child == NULL)
        return 0;
    child->save_result = save_result;

    if (self->col_cancelling)
        /* XXX FIXME this is a hack! */
        child->cancel_scope->cancelled = 1;

    PyCritical_Enter(self->crit);

    if (self->col_state != BRANCH_ALIVE) {
        exc = PyExc_TypeError;
        format = "%s() called in wrong state";
        goto failed;
    }

    if (PyState_Get()->import_depth)
        Py_FatalError("importing is not thread-safe");

    PyLinkedList_Append(&self->children, child);
    PyLinkedList_Append(&self->alive, child);

    if (PyThread_start_new_thread(NULL, branch_threadbootstrap, child) < 0) {
        exc = PyExc_RuntimeError;
        format = "%s can't spawn new thread";
        goto failed;
    }

    PyCritical_Exit(self->crit);
    return 1;

failed:
    if (!PyLinkedList_Detached(&child->children_links))
        PyLinkedList_Remove(&child->children_links);
    if (!PyLinkedList_Detached(&child->alive_links))
        PyLinkedList_Remove(&child->alive_links);
    PyCritical_Exit(self->crit);

    if (child->pystate)
        _PyState_Delete(child->pystate);
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
    PyCancelQueue queue;
    int run_queue = 0;
    PyBranchChild *child = (PyBranchChild *)arg;
    PyState_EnterFrame enterframe;
    PyBranchObject *branch = child->branch;
    int delete_child = 0;

    if (_PyState_EnterPreallocated(&enterframe, child->pystate)) {
        /* Because we preallocate everything, it should be
         * impossible to fail. */
        Py_FatalError("PyState_EnterPreallocated failed");
    }

    Py_INCREF(branch);

    _PyMonitorSpace_BlockOnSelf(&child->waitfor);

    PyCancel_Push(child->cancel_scope);

    child->result = PyObject_Call(child->func, child->args, child->kwds);
    if (!PyArg_RequireShareableReturn("branch._threadbootstrap",
            child->func, child->result))
        Py_CLEAR(child->result);

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

    PyCritical_Enter(branch->crit);

    if (child->result != NULL) {
        if (child->save_result)
            branch->col_resultcount++;
        else {
            /* XXX child->result was DECREF'd earlier */
            child->result = NULL;
            delete_child = 1;
        }
    } else {
        branch->col_exceptioncount++;
        if (branch->col_exceptioncount == 1) {
            PyBranchChild *otherchild = NULL;

            PyCancelQueue_Init(&queue);
            while (PyLinkedList_Next(&branch->alive, &otherchild))
                PyCancelQueue_Cancel(&queue, otherchild->cancel_scope);
            run_queue = 1;
        }
    }

    PyCritical_Exit(branch->crit);

    if (run_queue)
        PyCancelQueue_Finish(&queue);

    _PyMonitorSpace_UnblockOnSelf(&child->waitfor);

    PyCritical_Enter(branch->crit);
    PyCancel_Pop(child->cancel_scope);

    PyLinkedList_Remove(&child->alive_links);

    if (delete_child)
        PyLinkedList_Append(&branch->deletable, child);

    PyThread_flag_set(child->dead);

    if (branch->col_state == BRANCH_ALIVE)
        branch_cleanchildren(branch);

    /* After the main thread regains the critical and finds no threads
     * left, it starts skipping the critical.  Therefor it's not safe to
     * touch the branch after we release this critical. */
    PyCritical_Exit(branch->crit);

    Py_DECREF(branch);

    _PyState_ExitPreallocated(&enterframe);
}

static void
branch_cleanchildren(PyBranchObject *self)
{
    /* Assumes self->crit is already held */

    while (!PyLinkedList_Empty(&self->deletable)) {
        PyBranchChild *child = PyLinkedList_First(&self->deletable);

        PyLinkedList_Remove(&child->children_links);
        PyLinkedList_Remove(&child->alive_links);

        BranchChild_Delete(child);
    }
}

static PyObject *
Branch_getresults(PyBranchObject *self)
{
    PyObject *results;
    PyBranchChild *child = NULL;
    Py_ssize_t i;

    PyCritical_Enter(self->crit);
    if (self->col_state != BRANCH_DEAD) {
        PyCritical_Exit(self->crit);
        PyErr_SetString(PyExc_TypeError, "branch.getresults() called in "
            "wrong state");
        return NULL;
    }
    PyCritical_Exit(self->crit);

    /* Once we know the state is BRANCH_DEAD we can rely on the
     * MonitorSpace's locking to protect us.  A branch is not shareable. */

    assert(!self->col_exceptioncount);

    results = PyList_New(self->col_resultcount);
    if (results == NULL)
        return NULL;

    i = 0;
    while (PyLinkedList_Next(&self->children, &child)) {
        assert(i < self->col_resultcount);

        assert(child->exception == NULL);
        assert(child->result != NULL);

        /* Copy across, stealing references */
        PyList_SET_ITEM(results, i, child->result);
        child->result = NULL;
        PyLinkedList_Append(&self->deletable, child);
        i++;
    }
    assert(i == self->col_resultcount);
    self->col_resultcount = 0;
    branch_cleanchildren(self);
    return results;
}

static void
Branch_raiseexception(PyBranchObject *self)
{
    PyBranchChild *child = NULL;
    PyObject *causes;
    PyObject *interesting = NULL;
    int multiple = 0;
    PyObject *type, *val, *tb;
    Py_ssize_t i;

    assert(self->col_state == BRANCH_DEAD);
    assert(self->col_resultcount == 0);
    assert(self->col_exceptioncount);

    causes = PyTuple_New(self->col_exceptioncount);
    if (causes == NULL) {
        while (PyLinkedList_Next(&self->children, &child)) {
            Py_CLEAR(child->exception);
            PyLinkedList_Append(&self->deletable, child);
        }
        self->col_exceptioncount = 0;
        branch_cleanchildren(self);

        PyErr_NoMemory();
        return;
    }

    i = 0;
    while (PyLinkedList_Next(&self->children, &child)) {
        assert(i < self->col_exceptioncount);

        assert(child->result == NULL);
        assert(child->exception != NULL);

        if (!PyErr_GivenExceptionMatches(child->exception,
                PyExc_Cancelled)) {
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
        Py_CLEAR(child->exception);
        PyLinkedList_Append(&self->deletable, child);
        i++;
    }

    if (interesting) {
        PyErr_SetObject((PyObject *)Py_TYPE(interesting),
            ((PyBaseExceptionObject *)interesting)->args);
        Py_DECREF(interesting);
    } else if (self->col_cancelling && !multiple)
        PyErr_SetNone(PyExc_Cancelled);
    else
        PyErr_SetNone(PyExc_MultipleError);

    PyErr_Fetch(&type, &val, &tb);
    PyErr_NormalizeException(&type, &val, &tb);
    PyException_SetCause(val, causes); /* Consumes a reference to causes */
    PyErr_Restore(type, val, tb);

    assert(i == self->col_exceptioncount);
    self->col_exceptioncount = 0;
    branch_cleanchildren(self);
    return;
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

