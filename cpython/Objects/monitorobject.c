
#include "Python.h"
#include "ceval.h"
#include "branchobject.h"
#include "cancelobject.h"
#include "monitorobject.h"

static PyObject *PyMonitorSpace_Enter(PyMonitorSpaceObject *self,
    PyObject *func, PyObject *args, PyObject *kwds, ternaryfunc call2);
static PyObject *PyMonitorSpace_Leave(PyMonitorSpaceObject *self,
    PyObject *func, PyObject *args, PyObject *kwds);

static int monitorspace_acquire(PyMonitorSpaceObject *self, int pushing);
static void monitorspace_release(PyMonitorSpaceObject *self, PyState *give_to);

typedef struct {
    PyObject_HEAD
    PyObject *cond_callable;
} condition;

typedef struct {
    PyObject_HEAD
    PyObject *cond;
    PyObject *monitor;
    PyLinkedList waiters;
} boundcondition;

static PyThread_type_lock *deadlock_lock;
static double deadlock_delay = 1.0;


/* MonitorMeta methods */
static void
MonitorMeta_dealloc(PyTypeObject *self)
{
    PyObject_Del(self);
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
    "_threadtoolsmodule.MonitorMeta",       /*tp_name*/
    sizeof(PyHeapTypeObject),               /*tp_basicsize*/
    0/*sizeof(PyMemberDef)*/,               /*tp_itemsize*/
    (destructor)MonitorMeta_dealloc,        /*tp_dealloc*/
    0,                                      /*tp_print*/
    0,                                      /*tp_getattr*/
    0,                                      /*tp_setattr*/
    0,                                      /*tp_compare*/
    0,                                      /*tp_repr*/
    0,                                      /*tp_as_number*/
    0,                                      /*tp_as_sequence*/
    0,                                      /*tp_as_mapping*/
    0,                                      /*tp_hash*/
    (ternaryfunc)MonitorMeta_call,          /*tp_call*/
    0,                                      /*tp_str*/
    PyObject_GenericGetAttr,                /*tp_getattro*/
    0,                                      /*tp_setattro*/
    0,                                      /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
            Py_TPFLAGS_BASETYPE |
            Py_TPFLAGS_SHAREABLE,           /*tp_flags*/
    0,                                      /*tp_doc*/
    (traverseproc)MonitorMeta_traverse,     /*tp_traverse*/
    0,                                      /*tp_clear*/
    0,                                      /*tp_richcompare*/
    0,                                      /*tp_weaklistoffset*/
    0,                                      /*tp_iter*/
    0,                                      /*tp_iternext*/
    0,                                      /*tp_methods*/
    0,                                      /*tp_members*/
    0,                                      /*tp_getset*/
    0,                                      /*tp_base*/
    0,                                      /*tp_dict*/
    0,                                      /*tp_descr_get*/
    0,                                      /*tp_descr_set*/
    0,                                      /*tp_dictoffset*/
    0,                                      /*tp_init*/
    0,                                      /*tp_new*/
};
/* --------------------------------------------------------------------- */


/* Monitor methods */
static void
Monitor_dealloc(PyMonitorObject *self)
{
    PyObject *monitorspace = self->mon_monitorspace;
    Py_DECREF(self->mon_conditions);
    PyObject_Del(self);
    Py_DECREF(monitorspace);
    assert(self->mon_waking == 0);
}

/* PyErr_Occurred() should always be used after this */
static void
monitor_recheck_conditions(PyMonitorObject *self, boundcondition *skipped_bcond)
{
    Py_ssize_t i = 0;
    PyObject *key, *value, *x;
    int res;

#warning XXX FIXME monitor_recheck_conditions should be visible in the traceback stack

    if (self->mon_waking)
        return;

    while (PyDict_NextEx(self->mon_conditions, &i, &key, &value)) {
        boundcondition *bcond = (boundcondition *)value;

        if (bcond != skipped_bcond && !PyLinkedList_Empty(&bcond->waiters)) {
            x = PyObject_CallFunction(((condition *)bcond->cond)->cond_callable,
                "O", self);
            res = PyObject_IsTrue(x);
            Py_DECREF(x);
            if (x < 0) {
                Py_DECREF(key);
                Py_DECREF(value);
                return;
            } else if (x > 0) {
                PyState *t = PyLinkedList_First(&bcond->waiters);

                PyThread_flag_set(t->condition_flag);
                self->mon_waking = 1;

                Py_DECREF(key);
                Py_DECREF(value);
                return;
            }
        }

        Py_DECREF(key);
        Py_DECREF(value);
    }
}

static int
Monitor_traverse(PyMonitorObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->mon_monitorspace);
    Py_VISIT(self->mon_conditions);
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

        x->mon_conditions = PyDict_New();
        if (x->mon_conditions == NULL) {
            Py_DECREF(x->mon_monitorspace);
            PyObject_Del(self);
            return NULL;
        }

        x->mon_waking = 0;
    }
    return self;
}

static PyObject *
Monitor_wait(PyMonitorObject *self, PyObject *args, PyObject *kwds)
{
    PyMonitorSpaceObject *monitorspace = PyMonitor_GetMonitorSpace(self);
    PyObject *func;
    PyObject *smallargs;
    PyObject *result;

    /* Monitor.wait isn't shareable, so it shouldn't be possible to call
     * us without being the current monitorspace.  Otherwise we'd have
     * to raise an exception here. */
    assert(PyMonitorSpace_IsCurrent(monitorspace));

    monitor_recheck_conditions(self, NULL);

    if (PyTuple_Size(args) < 1) {
        PyErr_SetString(PyExc_TypeError,
            "Monitor.__wait__() needs a function to be called");
        return NULL;
    }

    func = PyTuple_GetItem(args, 0);

    smallargs = PyTuple_GetSlice(args, 1, PyTuple_Size(args));
    if (smallargs == NULL) {
        return NULL;
    }

    result = PyMonitorSpace_Leave(monitorspace, func, smallargs, kwds);
    Py_DECREF(smallargs);

    return result;
}

static int
Monitor_isshareable (PyObject *self)
{
    return 1;
}

static PyObject *
Monitor_monitorspace(PyMonitorObject *self, void *context)
{
    Py_INCREF(self->mon_monitorspace);
    return self->mon_monitorspace;
}

PyDoc_STRVAR(Monitor_wait__doc__, "__wait__(func, *args, **kwargs) -> object");

static PyMethodDef Monitor_methods[] = {
    {"__wait__", (PyCFunction)Monitor_wait, METH_VARARGS | METH_KEYWORDS,
        Monitor_wait__doc__},
    {NULL, NULL}                /* sentinel */
};

static PyGetSetDef Monitor_getset[] = {
    {"__monitorspace__", (getter)Monitor_monitorspace, NULL, NULL},
    {0}
};

PyTypeObject PyMonitor_Type = {
    PyVarObject_HEAD_INIT(&PyMonitorMeta_Type, 0)
    "_threadtoolsmodule.Monitor",           /*tp_name*/
    sizeof(PyMonitorObject),                /*tp_basicsize*/
    0,                                      /*tp_itemsize*/
    (destructor)Monitor_dealloc,            /*tp_dealloc*/
    0,                                      /*tp_print*/
    0,                                      /*tp_getattr*/
    0,                                      /*tp_setattr*/
    0,                                      /*tp_compare*/
    0,                                      /*tp_repr*/
    0,                                      /*tp_as_number*/
    0,                                      /*tp_as_sequence*/
    0,                                      /*tp_as_mapping*/
    0,                                      /*tp_hash*/
    0,                                      /*tp_call*/
    0,                                      /*tp_str*/
    PyObject_GenericGetAttr,                /*tp_getattro*/
    0,                                      /*tp_setattro*/
    0,                                      /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
            Py_TPFLAGS_BASETYPE | Py_TPFLAGS_MONITOR_SUBCLASS |
            Py_TPFLAGS_SHAREABLE,           /*tp_flags*/
    0,                                      /*tp_doc*/
    (traverseproc)Monitor_traverse,         /*tp_traverse*/
    0,                                      /*tp_clear*/
    0,                                      /*tp_richcompare*/
    0,                                      /*tp_weaklistoffset*/
    0,                                      /*tp_iter*/
    0,                                      /*tp_iternext*/
    Monitor_methods,                        /*tp_methods*/
    0,                                      /*tp_members*/
    Monitor_getset,                         /*tp_getset*/
    0,                                      /*tp_base*/
    0,                                      /*tp_dict*/
    0,                                      /*tp_descr_get*/
    0,                                      /*tp_descr_set*/
    0,                                      /*tp_dictoffset*/
    0,                                      /*tp_init*/
    Monitor_new,                            /*tp_new*/
    0,                                      /*tp_is_gc*/
    0,                                      /*tp_bases*/
    0,                                      /*tp_mro*/
    0,                                      /*tp_cache*/
    0,                                      /*tp_subclasses*/
    0,                                      /*tp_weaklist*/
    Monitor_isshareable,                    /*tp_isshareable*/
};
/* --------------------------------------------------------------------- */


/* monitormethod methods */
typedef struct {
    PyObject_HEAD
    PyObject *mm_callable;
} monitormethod;

static void
mm_dealloc(monitormethod *mm)
{
    Py_DECREF(mm->mm_callable);
    PyObject_Del(mm);
}

static int
mm_traverse(monitormethod *mm, visitproc visit, void *arg)
{
    Py_VISIT(mm->mm_callable);
    return 0;
}

static PyObject *
mm_descr_get(PyObject *self, PyObject *obj, PyObject *type)
{
    monitormethod *mm = (monitormethod *)self;

    return PyBoundMonitorMethod_New(mm->mm_callable, obj);
}

static PyObject *
mm_new_common(PyTypeObject *type, PyObject *callable)
{
    monitormethod *mm;

    if (!PyCallable_Check(callable)) {
        PyErr_Format(PyExc_TypeError, "'%s' object is not callable",
            Py_TYPE(callable)->tp_name);
        return NULL;
    }

    mm = PyObject_New(type);
    if (mm == NULL)
        return NULL;

    Py_INCREF(callable);
    mm->mm_callable = callable;

    return (PyObject *)mm;
}

PyObject *
PyMonitorMethod_New(PyObject *callable)
{
    return mm_new_common(&PyMonitorMethod_Type, callable);
}

static PyObject *
mm_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *callable;

    if (!PyArg_UnpackTuple(args, "monitormethod", 1, 1, &callable))
        return NULL;
    if (!_PyArg_NoKeywords("monitormethod", kwds))
        return NULL;

    return mm_new_common(type, callable);
}

static int
mm_isshareable(monitormethod *mm)
{
    return PyObject_IsShareable(mm->mm_callable);
}

PyDoc_STRVAR(monitormethod_doc,
"monitormethod(function) -> method\n\
\n\
Convert a function to be a monitor method.\n\
\n\
A monitor method enters the monitor when called.");

PyTypeObject PyMonitorMethod_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "monitormethod",
    sizeof(monitormethod),
    0,
    (destructor)mm_dealloc,                     /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
            Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_SHAREABLE,
    monitormethod_doc,                          /* tp_doc */
    (traverseproc)mm_traverse,                  /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    0,                                          /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    mm_descr_get,                               /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    mm_new,                                     /* tp_new */
    0,                                          /* tp_is_gc */
    0,                                          /* tp_bases */
    0,                                          /* tp_mro */
    0,                                          /* tp_cache */
    0,                                          /* tp_subclasses */
    0,                                          /* tp_weaklist */
    (isshareablefunc)mm_isshareable,            /* tp_isshareable */
};
/* --------------------------------------------------------------------- */


/* boundmonitormethod methods */
typedef struct {
    PyObject_HEAD
    PyObject *bmm_callable;
    PyObject *bmm_self;
} boundmonitormethod;

static void
bmm_dealloc(boundmonitormethod *bmm)
{
    Py_DECREF(bmm->bmm_callable);
    Py_DECREF(bmm->bmm_self);
    PyObject_Del(bmm);
}

static int
bmm_traverse(boundmonitormethod *bmm, visitproc visit, void *arg)
{
    Py_VISIT(bmm->bmm_callable);
    Py_VISIT(bmm->bmm_self);
    return 0;
}

static PyObject *
bmm_new_common(PyTypeObject *type, PyObject *callable, PyObject *self)
{
    boundmonitormethod *bmm;

    if (!PyCallable_Check(callable)) {
        PyErr_Format(PyExc_TypeError, "'%s' object is not callable",
            Py_TYPE(callable)->tp_name);
        return NULL;
    }
    if (!PyMonitor_Check(self)) {
        PyErr_Format(PyExc_TypeError, "'%s' object is not a Monitor",
            Py_TYPE(self)->tp_name);
        return NULL;
    }

    bmm = PyObject_New(type);
    if (bmm == NULL)
        return NULL;

    Py_INCREF(callable);
    bmm->bmm_callable = callable;
    Py_INCREF(self);
    bmm->bmm_self = self;

    return (PyObject *)bmm;
}

PyObject *
PyBoundMonitorMethod_New(PyObject *callable, PyObject *self)
{
    return bmm_new_common(&PyBoundMonitorMethod_Type, callable, self);
}

static PyObject *
bmm_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *callable;
    PyObject *self;

    if (!PyArg_UnpackTuple(args, "boundmonitormethod", 2, 2, &callable, &self))
        return NULL;
    if (!_PyArg_NoKeywords("boundmonitormethod", kwds))
        return NULL;

    return bmm_new_common(type, callable, self);
}

static PyObject *
bmm_call_inner(PyObject *result, PyObject *arg, PyObject *kw)
{
    PyMonitorObject *monitor;
    assert(PyTuple_Size(arg) >= 1);
    monitor = (PyMonitorObject *)PyTuple_GET_ITEM(arg, 0);
    assert(PyMonitor_Check(monitor));

    monitor_recheck_conditions(monitor, NULL);
    if (PyErr_Occurred())
        return NULL;
    Py_XINCREF(result);
    return result;
}

static PyObject *
bmm_call(boundmonitormethod *bmm, PyObject *arg, PyObject *kw)
{
    PyObject *result;
    Py_ssize_t argcount = PyTuple_Size(arg);
    PyObject *newarg = PyTuple_New(argcount + 1);
    int i;
    PyMonitorSpaceObject *monitorspace;

    if (newarg == NULL)
        return NULL;
    Py_INCREF(bmm->bmm_self);
    PyTuple_SET_ITEM(newarg, 0, bmm->bmm_self);
    for (i = 0; i < argcount; i++) {
        PyObject *v = PyTuple_GET_ITEM(arg, i);
        Py_XINCREF(v);
        PyTuple_SET_ITEM(newarg, i+1, v);
    }
    arg = newarg;

    monitorspace = PyMonitor_GetMonitorSpace(bmm->bmm_self);

    if (PyMonitorSpace_IsCurrent(monitorspace))
        result = PyEval_CallObjectWithKeywords(bmm->bmm_callable, arg,
            kw);
    else
        result = PyMonitorSpace_Enter(monitorspace, bmm->bmm_callable,
            arg, kw, bmm_call_inner);
    Py_DECREF(arg);
    return result;
}

static int
bmm_isshareable(boundmonitormethod *bmm)
{
    return PyObject_IsShareable(bmm->bmm_callable) &&
        PyObject_IsShareable(bmm->bmm_self);
}

PyDoc_STRVAR(boundmonitormethod_doc,
"boundmonitormethod(function, self) -> method\n\
\n\
Convert a function and a monitor to be a bound monitor method.\n\
\n\
A bound monitor method enters the monitor when called.");

PyTypeObject PyBoundMonitorMethod_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "boundmonitormethod",
    sizeof(boundmonitormethod),
    0,
    (destructor)bmm_dealloc,                    /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    (ternaryfunc)bmm_call,                      /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
            Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_SHAREABLE,
    boundmonitormethod_doc,                     /* tp_doc */
    (traverseproc)bmm_traverse,                 /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    0,                                          /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    bmm_new,                                    /* tp_new */
    0,                                          /* tp_is_gc */
    0,                                          /* tp_bases */
    0,                                          /* tp_mro */
    0,                                          /* tp_cache */
    0,                                          /* tp_subclasses */
    0,                                          /* tp_weaklist */
    (isshareablefunc)bmm_isshareable,           /* tp_isshareable */
};
/* --------------------------------------------------------------------- */


/* condition methods */
static void
cond_dealloc(condition *cond)
{
    Py_DECREF(cond->cond_callable);
    PyObject_Del(cond);
}

static int
cond_traverse(condition *cond, visitproc visit, void *arg)
{
    Py_VISIT(cond->cond_callable);
    return 0;
}

static PyObject *
cond_descr_get(PyObject *self, PyObject *obj, PyObject *type)
{
    PyObject *bcond;
    PyMonitorObject *mon = (PyMonitorObject *)obj;

    if (!PyMonitor_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "'%s' object is not a Monitor",
            Py_TYPE(obj)->tp_name);
        return NULL;
    }

    if (PyDict_GetItemEx(mon->mon_conditions, self, &bcond) < 0)
        return NULL;
    if (bcond != NULL)
        return bcond;

    bcond = PyBoundMonitorCondition_New(self, obj);
    if (bcond == NULL)
        return NULL;

    if (PyDict_SetItem(mon->mon_conditions, self, bcond)) {
        Py_DECREF(bcond);
        return NULL;
    }

    return bcond;
}

static PyObject *
cond_new_common(PyTypeObject *type, PyObject *callable)
{
    condition *cond;

    if (!PyCallable_Check(callable)) {
        PyErr_Format(PyExc_TypeError, "'%s' object is not callable",
            Py_TYPE(callable)->tp_name);
        return NULL;
    }

    cond = PyObject_New(type);
    if (cond == NULL)
        return NULL;

    Py_INCREF(callable);
    cond->cond_callable = callable;

    return (PyObject *)cond;
}

PyObject *
PyMonitorCondition_New(PyObject *callable)
{
    return cond_new_common(&PyMonitorCondition_Type, callable);
}

static PyObject *
cond_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *callable;

    if (!PyArg_UnpackTuple(args, "condition", 1, 1, &callable))
        return NULL;
    if (!_PyArg_NoKeywords("condition", kwds))
        return NULL;

    return cond_new_common(type, callable);
}

static int
cond_isshareable(condition *cond)
{
    return PyObject_IsShareable(cond->cond_callable);
}

PyDoc_STRVAR(condition_doc,
"condition(function) -> condition\n\
\n\
Convert a function to be a monitor condition.\n\
\n\
A monitor condition allows waiting for a property of the monitor\n\
to become true, using wait(mon.condition).");

PyTypeObject PyMonitorCondition_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "condition",
    sizeof(condition),
    0,
    (destructor)cond_dealloc,                     /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    (hashfunc)_Py_HashPointer,                  /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
            Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_SHAREABLE,
    condition_doc,                              /* tp_doc */
    (traverseproc)cond_traverse,                /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    0,                                          /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    cond_descr_get,                             /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    cond_new,                                   /* tp_new */
    0,                                          /* tp_is_gc */
    0,                                          /* tp_bases */
    0,                                          /* tp_mro */
    0,                                          /* tp_cache */
    0,                                          /* tp_subclasses */
    0,                                          /* tp_weaklist */
    (isshareablefunc)cond_isshareable,          /* tp_isshareable */
};
/* --------------------------------------------------------------------- */


/* boundcondition methods */
static void
bcond_dealloc(boundcondition *bcond)
{
    Py_DECREF(bcond->cond);
    Py_DECREF(bcond->monitor);
    assert(PyLinkedList_Empty(&bcond->waiters));
    PyObject_Del(bcond);
}

static int
bcond_traverse(boundcondition *bcond, visitproc visit, void *arg)
{
    Py_VISIT(bcond->cond);
    Py_VISIT(bcond->monitor);
    return 0;
}

PyObject *
PyBoundMonitorCondition_New(PyObject *cond, PyObject *mon)
{
    boundcondition *bcond;

    assert(PyCondition_Check(cond));
    assert(PyMonitor_Check(mon));

    bcond = PyObject_New(&PyBoundMonitorCondition_Type);
    if (bcond == NULL)
        return NULL;

    Py_INCREF(cond);
    bcond->cond = cond;
    Py_INCREF(mon);
    bcond->monitor = mon;
    PyLinkedList_InitBase(&bcond->waiters, offsetof(PyState, condition_links));

    return (PyObject *)bcond;
}

/* XXX FIXME clean up the Cancel API and delete this. */
typedef struct {
    PyState *pystate;
    int cancelled;
} blewed_up;

static void
boundcondition_wakeup(PyCancelQueue *queue, void *arg)
{
    blewed_up *bu = arg;
    bu->cancelled = 1;
    PyThread_flag_set(bu->pystate->condition_flag);
}

static PyObject *
boundcondition_wait(boundcondition *bcond)
{
    PyState *pystate = PyState_Get();
    PyMonitorSpaceObject *monitorspace =
        PyMonitor_GetMonitorSpace(bcond->monitor);
    PyCancelObject *cancel_scope;
    PyObject *x, *result = NULL;
    int res;
    blewed_up bu;

    /* boundconditions aren't shareable, so it shouldn't be possible to
     * call us without being the current monitorspace.  Otherwise we'd
     * have to raise an exception here. */
    assert(PyMonitorSpace_IsCurrent(monitorspace));

    bu.pystate = pystate;
    bu.cancelled = 0;

    cancel_scope = PyCancel_New(boundcondition_wakeup, &bu, pystate);
    if (cancel_scope == NULL)
        return NULL;

    /* Push it briefly in case we're called when cancelled */
    PyCancel_Push(cancel_scope);
    PyCancel_Pop(cancel_scope);

    while (1) {
        if (bu.cancelled) {
            PyErr_SetString(PyExc_Cancelled,
                "condition.__wait__ cancelled");
            break;
        }

        /* If the condition is already true we don't need to wait */
        x = PyObject_CallFunction(((condition *)bcond->cond)->cond_callable,
            "O", bcond->monitor);
        if (x == NULL)
            break;
        res = PyObject_IsTrue(x);
        Py_DECREF(x);
        if (res < 0)
            break;
        if (res > 0) {
            Py_INCREF(Py_None);
            result = Py_None;
            break;
        }

        /* Otherwise we put ourselves to sleep */
        PyLinkedList_Append(&bcond->waiters, pystate);
        monitor_recheck_conditions((PyMonitorObject *)bcond->monitor, bcond);

        PyCancel_Push(cancel_scope);
        monitorspace_release(monitorspace, NULL);
        /* XXX FIXME be cancellable! */
        PyState_Suspend();
        PyThread_flag_wait(pystate->condition_flag);
        PyState_Resume();
        if (monitorspace_acquire(monitorspace, 0))
            Py_FatalError("condition.__wait__ unable to reacquire MonitorSpace");
        PyCancel_Pop(cancel_scope);

        PyThread_flag_clear(pystate->condition_flag);
        ((PyMonitorObject *)bcond->monitor)->mon_waking = 0;

        PyLinkedList_Remove(&pystate->condition_links);

        /* monitor_recheck_conditions and monitorspace_acquire may both
         * set exceptions */
        if (PyErr_Occurred())
            break;
    }

    PyThread_flag_clear(pystate->condition_flag);

    Py_DECREF(cancel_scope);
    return result;
}

static PyMethodDef boundcondition_methods[] = {
    {"__wait__", (PyCFunction)boundcondition_wait, METH_NOARGS,
            NULL},
    {NULL, NULL}                /* sentinel */
};

PyDoc_STRVAR(boundcondition_doc,
"A monitor condition allows waiting for a property of the monitor\n\
to become true, using wait(mon.condition).");

PyTypeObject PyBoundMonitorCondition_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "boundcondition",
    sizeof(boundcondition),
    0,
    (destructor)bcond_dealloc,                  /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_SHAREABLE,
    boundcondition_doc,                         /* tp_doc */
    (traverseproc)bcond_traverse,               /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    boundcondition_methods,                     /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_new */
    0,                                          /* tp_is_gc */
    0,                                          /* tp_bases */
    0,                                          /* tp_mro */
    0,                                          /* tp_cache */
    0,                                          /* tp_subclasses */
    0,                                          /* tp_weaklist */
    0,                                          /* tp_isshareable */
};
/* --------------------------------------------------------------------- */


/* MonitorSpace methods */
void
PyMonitorSpace_SetDeadlockDelay(double delay)
{
    PyState_StopTheWorld();
    deadlock_delay = delay;
    PyState_StartTheWorld();
}

double
PyMonitorSpace_GetDeadlockDelay(void)
{
    return deadlock_delay;
}

static void
inspect_init(PyWaitFor_Inspection *insp, int global)
{
    PyLinkedList_InitBase(&insp->inspecting,
        offsetof(PyWaitFor, inspection_links));
    insp->global = global;
}

static void
inspect_add(PyWaitFor_Inspection *insp, PyWaitFor *node)
{
    PyThread_lock_acquire(node->lock);
    PyLinkedList_Append(&insp->inspecting, node);
    if (insp->global) {
        assert(node->checking_deadlock == 0);
        node->checking_deadlock = 1;
    }
}

static int
inspect_tryadd(PyWaitFor_Inspection *insp, PyWaitFor *node)
{
    assert(!insp->global);
    if (PyThread_lock_tryacquire(node->lock)) {
        PyLinkedList_Append(&insp->inspecting, node);
        return 1;
    } else
        return 0;
}

static void
inspect_cond_timedwait(PyWaitFor_Inspection *insp, PyWaitFor *node,
    PyThread_type_cond *cond, PyThread_type_timeout *timeout)
{
    assert(!insp->global);

    PyLinkedList_Remove(&node->inspection_links);
    if (!PyLinkedList_Empty(&insp->inspecting))
        Py_FatalError("inspect_cond_timedwait only works when inspecting a single node");
    PyThread_cond_timedwait(cond, node->lock, timeout);
    PyLinkedList_Append(&insp->inspecting, node);
}

static void
inspect_clear(PyWaitFor_Inspection *insp)
{
    while (!PyLinkedList_Empty(&insp->inspecting)) {
        PyWaitFor *node = PyLinkedList_First(&insp->inspecting);
        PyLinkedList_Remove(&node->inspection_links);
        if (insp->global) {
            assert(node->checking_deadlock);
            node->checking_deadlock = 0;
        }
        PyThread_lock_release(node->lock);
    }
}

/* XXX FIXME rename */
/* This returns with a and b both locked.  This lets the caller modify
 * them knowing they *currently* don't deadlock. */
static int
deadlock_check_partial(PyWaitFor_Inspection *insp, PyWaitFor *a, PyWaitFor *b)
{
    PyWaitFor *node;

    inspect_add(insp, a);
    inspect_add(insp, b);

    /* If we find a NULL blocker then we know there can't be a deadlock */
    node = b->blocker;
    while (1) {
        if (node == NULL)
            return 0;

        if (!inspect_tryadd(insp, node))
            break;

        node = node->blocker;
    }

    /* Otherwise we setup for a full check */
    inspect_clear(insp);
    inspect_init(insp, 1);

    PyThread_lock_acquire(deadlock_lock);

    inspect_add(insp, a);
    inspect_add(insp, b);

    return 1;
}

/* We'll already have origin locked, as provided by deadlock_check_partial */
static void
deadlock_check_full(PyWaitFor_Inspection *insp, PyWaitFor *origin)
{
    PyWaitFor *node = origin->blocker;

    /* Confirm that there really is a deadlock */
    while (1) {
        if (node == NULL)
            return;

        if (node->checking_deadlock)
            break;

        inspect_add(insp, node);
        node = node->blocker;
    }

    /* Now go break it */
    node = origin;
    while (1) {
        assert(node != NULL);

        if (node->abortfunc != NULL) {
            node->abortfunc(insp, node);
            break;
        }

        node = node->blocker;
        if (node == origin)
            /* XXX FIXME this should eventually be be replaced with
             * a second pass that attempts to preempt resources.
             * That'll only be required once Monitor.wait() is added
             * along with MonitorSpace.localenter(). */
            Py_FatalError("Unable to break deadlock");
    }
}

static void
monitorspace_abortfunc(PyWaitFor_Inspection *insp, PyWaitFor *node)
{
    PyState *pystate = node->self;

    PyLinkedList_Remove(&pystate->monitorspace_waitinglinks);
    pystate->waitfor.blocker = NULL;
    pystate->waitfor.abortfunc = NULL;
    PyThread_flag_set(pystate->monitorspace_waitingflag);
}

/* 0 indicates you got the lock, 1 indicates you failed.  Note that an
 * exception may be set even if you got the lock, if pushing is not
 * set. */
static int
monitorspace_acquire(PyMonitorSpaceObject *self, int pushing)
{
    PyState *pystate = PyState_Get();
    PyWaitFor *resource = &self->waitfor;
    PyWaitFor_Inspection insp;
    int check_deadlock = 0;

    inspect_init(&insp, 0);

    inspect_add(&insp, resource);
    if (resource->blocker == NULL) {
        /* Fast path.  If completely uncontended we won't even context
         * switch once (assuming futexes on Linux). */
        resource->blocker = &pystate->waitfor;
        inspect_clear(&insp);
        return 0;
    }

    PyThread_timeout_set(pystate->monitorspace_timeout, deadlock_delay);
    while (resource->blocker != NULL) {
        /* Slightly less fast path.  Deadlock detection is a bottleneck,
         * so we putz around here first. */
        PyState_Suspend();
        inspect_cond_timedwait(&insp, resource, self->idle, pystate->monitorspace_timeout);
        PyState_Resume();
        if (PyThread_timeout_expired(pystate->monitorspace_timeout))
            break;
    }
    if (resource->blocker == NULL) {
        resource->blocker = &pystate->waitfor;
        inspect_clear(&insp);
        return 0;
    }
    inspect_clear(&insp);

    /* Non-global check for deadlocks.  May give a false-positive, but
     * never a false-negative.
     *
     * It also locks the two nodes given to it, giving us a safe window
     * to mark ourselves as blocked on them. */
    check_deadlock = deadlock_check_partial(&insp, &pystate->waitfor,
        resource);

    PyLinkedList_Append(&self->waiters, pystate);
    pystate->waitfor.blocker = resource;
    if (pushing)
        pystate->waitfor.abortfunc = monitorspace_abortfunc;
    else
        assert(pystate->waitfor.abortfunc == NULL);

    if (check_deadlock)
        deadlock_check_full(&insp, resource);

    while (resource->blocker != NULL) {
        inspect_clear(&insp);
        inspect_init(&insp, 0);
        if (check_deadlock) {
            PyThread_lock_release(deadlock_lock);
            check_deadlock = 0;
        }

        PyState_Suspend();
        PyThread_flag_wait(pystate->monitorspace_waitingflag);
        PyState_Resume();

        inspect_add(&insp, &pystate->waitfor);
        inspect_add(&insp, resource);

        PyThread_flag_clear(pystate->monitorspace_waitingflag);

        if (pystate->waitfor.blocker == NULL) {
            /* A deadlock was found.  We drew the short straw. */
            assert(PyLinkedList_Detached(&pystate->monitorspace_waitinglinks));
            inspect_clear(&insp);
            PyErr_SetString(PyExc_SoftDeadlockError, "monitor entrance "
                "failed due to deadlock");
            return 1;
        }
    }

    PyLinkedList_Remove(&pystate->monitorspace_waitinglinks);
    pystate->waitfor.blocker = NULL;
    assert(resource->blocker == NULL);
    pystate->waitfor.abortfunc = NULL;
    resource->blocker = &pystate->waitfor;

    inspect_clear(&insp);
    if (check_deadlock)
        PyThread_lock_release(deadlock_lock);

    return 0;
}

static void
monitorspace_release(PyMonitorSpaceObject *self, PyState *give_to)
{
    PyWaitFor *resource = &self->waitfor;
    PyWaitFor_Inspection insp;
    inspect_init(&insp, 0);

    if (give_to != NULL) {
        inspect_add(&insp, &give_to->waitfor);
        inspect_add(&insp, resource);
        assert(resource->blocker == &PyState_Get()->waitfor);
        assert(give_to->waitfor.blocker == resource);

        resource->blocker = &give_to->waitfor;
        PyLinkedList_Remove(&give_to->monitorspace_waitinglinks);
        give_to->waitfor.blocker = NULL;
        PyThread_flag_set(give_to->monitorspace_waitingflag);

        inspect_clear(&insp);
        return;
    }

    inspect_add(&insp, resource);
    assert(resource->blocker == &PyState_Get()->waitfor);

    resource->blocker = NULL;

    if (!PyLinkedList_Empty(&self->waiters)) {
        PyState *first = PyLinkedList_First(&self->waiters);
        PyThread_flag_set(first->monitorspace_waitingflag);
    } else
        /* XXX This could be improved having a count of the threads
         * waiting on self->idle as well as a flag indicating that one
         * is waking.  That could reduce unnecessary context switches in
         * the event that one thread is quickly acquiring/releasing the
         * monitor, faster than other threads can wake up.
         *
         * Or just use another linked list with the
         * monitorspace_waitingflag flag... */
        PyThread_cond_wakeone(self->idle);

    inspect_clear(&insp);
}

void
_PyMonitorSpace_WaitForBranchChild(PyBranchChild *self)
{
    PyState *pystate = PyState_Get();
    PyWaitFor *resource = &self->waitfor;
    PyWaitFor_Inspection insp;
    int check_deadlock = 0;

    inspect_init(&insp, 0);

    /* Fast path.  For short-lived threads we'll never need to do
     * deadlock detection. */
    PyState_Suspend();
    if (PyThread_flag_timedwait(self->dead, deadlock_delay)) {
        /* success! */
        PyState_Resume();
        return;
    }
    PyState_Resume();

    /* Non-global check for deadlocks.  May give a false-positive, but
     * never a false-negative.
     *
     * It also locks the two nodes given to it, giving us a safe window
     * to mark ourselves as blocked on them. */
    check_deadlock = deadlock_check_partial(&insp, &pystate->waitfor,
        resource);

    pystate->waitfor.blocker = resource;
    assert(pystate->waitfor.abortfunc == NULL);

    if (check_deadlock)
        deadlock_check_full(&insp, resource);

    inspect_clear(&insp);
    inspect_init(&insp, 0);
    if (check_deadlock) {
        PyThread_lock_release(deadlock_lock);
        check_deadlock = 0;
    }

    PyState_Suspend();
    PyThread_flag_wait(self->dead);
    PyState_Resume();

    inspect_add(&insp, &pystate->waitfor);
    inspect_add(&insp, resource);

    pystate->waitfor.blocker = NULL;
    assert(resource->blocker == NULL);

    inspect_clear(&insp);
    if (check_deadlock)
        PyThread_lock_release(deadlock_lock);
}

/* Marks the given node as blocked on the current thread.  Requires the
 * current thread to not be blocked on anything. */
void
_PyMonitorSpace_BlockOnSelf(PyWaitFor *node)
{
    PyWaitFor_Inspection insp;
    PyState *pystate = PyState_Get();

    inspect_init(&insp, 0);

    inspect_add(&insp, node);
    inspect_add(&insp, &pystate->waitfor);

    assert(node->blocker == NULL);
    assert(pystate->waitfor.blocker == NULL);

    node->blocker = &pystate->waitfor;

    inspect_clear(&insp);
}

void
_PyMonitorSpace_UnblockOnSelf(PyWaitFor *node)
{
    PyWaitFor_Inspection insp;
    PyState *pystate = PyState_Get();

    inspect_init(&insp, 0);

    inspect_add(&insp, node);
    inspect_add(&insp, &pystate->waitfor);

    assert(node->blocker == &pystate->waitfor);
    assert(pystate->waitfor.blocker == NULL);

    node->blocker = NULL;

    inspect_clear(&insp);
}

static PyObject *
monitorspace_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *self;

    assert(type != NULL);
    self = PyObject_New(type);
    if (self != NULL) {
        PyMonitorSpaceObject *x = (PyMonitorSpaceObject *)self;
        x->waitfor.lock = PyThread_lock_allocate();
        if (x->waitfor.lock == NULL) {
            PyObject_Del(self);
            PyErr_NoMemory();
            return NULL;
        }
        //x->lock_holder = NULL;
        //x->first_waiter = NULL;
        //x->last_waiter = NULL;
        x->idle = PyThread_cond_allocate();
        if (x->idle == NULL) {
            PyThread_lock_free(x->waitfor.lock);
#warning XXX FIXME not safe to call PyObject_Del in *_new
            PyObject_Del(self);
            PyErr_NoMemory();
            return NULL;
        }
        x->waitfor.self = self;
        x->waitfor.blocker = NULL;
        PyLinkedList_InitBase(&x->waiters, offsetof(PyState, monitorspace_waitinglinks));
        x->waitfor.checking_deadlock = 0;
        x->waitfor.abortfunc = NULL;
        PyLinkedList_InitNode(&x->waitfor.inspection_links);
    }
    return self;
}

static void
monitorspace_dealloc(PyMonitorSpaceObject *self)
{
    //assert(self->lock_holder == NULL);
    //assert(self->first_waiter == NULL);
    //assert(self->last_waiter == NULL);
    assert(self->waitfor.blocker == NULL);
    assert(PyLinkedList_Empty(&self->waiters));
    assert(self->waitfor.checking_deadlock == 0);
    assert(PyLinkedList_Detached(&self->waitfor.inspection_links));
    PyThread_lock_free(self->waitfor.lock);
    PyThread_cond_free(self->idle);
    PyObject_Del(self);
}

static PyObject *
monitorspace_enter(PyMonitorSpaceObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *func;
    PyObject *smallargs;
    PyObject *result;

    if (PyTuple_Size(args) < 1) {
        PyErr_SetString(PyExc_TypeError,
            "MonitorSpace.enter() needs a function to be called");
        return NULL;
    }

    func = PyTuple_GetItem(args, 0);

    smallargs = PyTuple_GetSlice(args, 1, PyTuple_Size(args));
    if (smallargs == NULL) {
        return NULL;
    }

    result = PyMonitorSpace_Enter(self, func, smallargs, kwds, NULL);
    Py_DECREF(smallargs);

    return result;
}

static PyObject *
PyMonitorSpace_Enter(PyMonitorSpaceObject *self, PyObject *func,
        PyObject *args, PyObject *kwds, ternaryfunc call2)
{
    PyObject *result;
    PyMonitorSpaceFrame frame;
    PyState *pystate = PyState_Get();

    if (!PyObject_IsShareable(func)) {
        PyErr_Format(PyExc_TypeError,
            "Function argument must be shareable, '%s' object "
            "is not", func->ob_type->tp_name);
        return NULL;
    }

    if (!PyArg_RequireShareable("MonitorSpace.enter", args, kwds))
        return NULL;

    if (pystate->critical_section != NULL)
        Py_FatalError("Cannot enter monitor while in a critical section");

    if (monitorspace_acquire(self, 1))
        return NULL;

    PyLinkedList_InitNode(&frame.links);
    PyLinkedList_Append(&pystate->monitorspaces, &frame);
    frame.monitorspace = self;

    result = PyEval_CallObjectWithKeywords(func, args, kwds);
    if (call2 != NULL) {
        PyObject *result2 = call2(result, args, kwds);
        Py_XDECREF(result);
        result = result2;
    }
    if (!PyArg_RequireShareableReturn("MonitorSpace.enter", func, result)) {
        Py_XDECREF(result);
        result = NULL;
    }

    assert(PyLinkedList_Last(&pystate->monitorspaces) == &frame);
    assert(frame.monitorspace == self);

    PyLinkedList_Remove(&frame.links);

    monitorspace_release(self, NULL);

    return result;
}

static PyObject *
monitorspace_leave(PyMonitorSpaceObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *func;
    PyObject *smallargs;
    PyObject *result;

    if (PyTuple_Size(args) < 1) {
        PyErr_SetString(PyExc_TypeError,
            "MonitorSpace.leave() needs a function to be called");
        return NULL;
    }

    func = PyTuple_GetItem(args, 0);

    smallargs = PyTuple_GetSlice(args, 1, PyTuple_Size(args));
    if (smallargs == NULL) {
        return NULL;
    }

    result = PyMonitorSpace_Leave(self, func, smallargs, kwds);
    Py_DECREF(smallargs);

    return result;
}

static PyObject *
PyMonitorSpace_Leave(PyMonitorSpaceObject *self, PyObject *func,
        PyObject *args, PyObject *kwds)
{
    PyObject *result;
    PyMonitorSpaceFrame frame;
    PyState *pystate = PyState_Get();

    if (!PyObject_IsShareable(func)) {
        PyErr_Format(PyExc_TypeError,
            "Function argument must be shareable, '%s' object "
            "is not", func->ob_type->tp_name);
        return NULL;
    }

    if (!PyArg_RequireShareable("MonitorSpace.leave", args, kwds))
        return NULL;

    if (pystate->critical_section != NULL)
        Py_FatalError("Cannot leave monitor while in a critical section");

    monitorspace_release(self, NULL);

    PyLinkedList_InitNode(&frame.links);
    PyLinkedList_Append(&pystate->monitorspaces, &frame);
    frame.monitorspace = NULL;

    result = PyEval_CallObjectWithKeywords(func, args, kwds);
    if (!PyArg_RequireShareableReturn("MonitorSpace.leave", func, result)) {
        Py_XDECREF(result);
        result = NULL;
    }

    assert(PyLinkedList_Last(&pystate->monitorspaces) == &frame);
#warning clearing frame.monitorspace does not release it
    Py_CLEAR(frame.monitorspace);

    PyLinkedList_Remove(&frame.links);

    if (monitorspace_acquire(self, 0))
        Py_FatalError("Non-pushing monitorspace_acquire failed");

    if (PyErr_Occurred())
        Py_CLEAR(result);
    return result;
}

int
PyMonitorSpace_IsCurrent(struct _PyMonitorSpaceObject *monitorspace)
{
    PyState *pystate = PyState_Get();
    PyMonitorSpaceFrame *frame;

    assert(monitorspace != NULL);
    frame = PyLinkedList_Last(&pystate->monitorspaces);
    return frame->monitorspace == monitorspace;
}

/* Returns a NEW reference */
PyObject *
PyMonitorSpace_GetCurrent(void)
{
    PyState *pystate = PyState_Get();
    PyMonitorSpaceFrame *frame;

    frame = PyLinkedList_Last(&pystate->monitorspaces);

#warning PyMonitorSpace_GetCurrent should use a critical section
    if (frame->monitorspace == NULL) {
        PyObject *monitorspace =
            PyObject_CallObject((PyObject *)&PyMonitorSpace_Type, NULL);
        if (monitorspace == NULL)
            return NULL;
#warning PyMonitorSpace_GetCurrent should acquire new monitor space
/* Should also make sure whoever clears frame->monitorspace releases it */
        assert(PyLinkedList_Last(&pystate->monitorspaces) == frame);
        assert(frame->monitorspace == NULL);
        frame->monitorspace = (PyMonitorSpaceObject *)monitorspace;
    }

    Py_INCREF(frame->monitorspace);
    return (PyObject *)frame->monitorspace;
}

static int
monitorspace_isshareable (PyObject *self)
{
    return 1;
}

PyDoc_STRVAR(monitorspace_enter__doc__, "enter(func, *args, **kwargs) -> object");
PyDoc_STRVAR(monitorspace_leave__doc__, "leave(func, *args, **kwargs) -> object");

static PyMethodDef monitorspace_methods[] = {
    {"enter", (PyCFunction)monitorspace_enter, METH_VARARGS | METH_KEYWORDS,
        monitorspace_enter__doc__},
    {"leave", (PyCFunction)monitorspace_leave, METH_VARARGS | METH_KEYWORDS,
        monitorspace_leave__doc__},
    {NULL, NULL}                            /* sentinel */
};

PyTypeObject PyMonitorSpace_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "_threadtoolsmodule.MonitorSpace",      /*tp_name*/
    sizeof(PyMonitorSpaceObject),           /*tp_basicsize*/
    0,                                      /*tp_itemsize*/
    (destructor)monitorspace_dealloc,       /*tp_dealloc*/
    0,                                      /*tp_print*/
    0,                                      /*tp_getattr*/
    0,                                      /*tp_setattr*/
    0,                                      /*tp_compare*/
    0,                                      /*tp_repr*/
    0,                                      /*tp_as_number*/
    0,                                      /*tp_as_sequence*/
    0,                                      /*tp_as_mapping*/
    0,                                      /*tp_hash*/
    0,                                      /*tp_call*/
    0,                                      /*tp_str*/
    PyObject_GenericGetAttr,                /*tp_getattro*/
    0,                                      /*tp_setattro*/
    0,                                      /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
            Py_TPFLAGS_SHAREABLE,           /*tp_flags*/
    0,                                      /*tp_doc*/
    0,                                      /*tp_traverse*/
    0,                                      /*tp_clear*/
    0,                                      /*tp_richcompare*/
    0,                                      /*tp_weaklistoffset*/
    0,                                      /*tp_iter*/
    0,                                      /*tp_iternext*/
    monitorspace_methods,                   /*tp_methods*/
    0,                                      /*tp_members*/
    0,                                      /*tp_getset*/
    0,                                      /*tp_base*/
    0,                                      /*tp_dict*/
    0,                                      /*tp_descr_get*/
    0,                                      /*tp_descr_set*/
    0,                                      /*tp_dictoffset*/
    0,                                      /*tp_init*/
    monitorspace_new,                       /*tp_new*/
    0,                                      /*tp_is_gc*/
    0,                                      /*tp_bases*/
    0,                                      /*tp_mro*/
    0,                                      /*tp_cache*/
    0,                                      /*tp_subclasses*/
    0,                                      /*tp_weaklist*/
    monitorspace_isshareable,               /*tp_isshareable*/
};


void
_PyMonitor_Init(void)
{
    deadlock_lock = PyThread_lock_allocate();
    if (!deadlock_lock)
        Py_FatalError("Failed to allocate deadlock_lock");
}
