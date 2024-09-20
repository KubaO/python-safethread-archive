#include "Python.h"
#include "structmember.h"


static int deathqueue_clear(PyDeathQueue *self);
static int PyDeathQueue_Cancel(PyDeathQueue *queue, PyDeathQueueHandle *handle);
static PyObject *deathqueue_wait(PyDeathQueue *queue);


static void
weakref_dealloc(PyWeakReference *self)
{
    if (self->wr_object != NULL)
        Py_FatalError("Still-valid weakref deleted!");
    PyCritical_Free(self->crit);
    PyObject_Del(self);
}

static int
weakref_traverse(PyWeakReference *self, visitproc visit, void *arg)
{
    return 0;
}

static PyObject *
weakref_call(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwlist[] = {NULL};
    PyObject *ob;

    if (!PyArg_ParseTupleAndKeywords(args, kw, ":__call__", kwlist))
        return NULL;

    ob = PyWeakref_GetObjectEx(self);
    if (ob == NULL) {
        Py_INCREF(Py_None);
        ob = Py_None;
    }

    return ob;
}

static PyObject *
weakref_repr(PyWeakReference *self)
{
    char buffer[256];
    PyObject *ob = PyWeakref_GetObjectEx((PyObject *)self);

    if (ob == NULL)
        PyOS_snprintf(buffer, sizeof(buffer), "<weakref at %p; dead>", self);
    else {
        char *name = NULL;
        PyObject *nameobj = PyObject_GetAttrString(ob, "__name__");

        if (nameobj == NULL)
            PyErr_Clear();
        else if (PyUnicode_Check(nameobj))
            name = PyUnicode_AsString(nameobj);

        PyOS_snprintf(buffer, sizeof(buffer),
                      name ? "<weakref at %p; to '%.50s' at %p (%s)>"
                           : "<weakref at %p; to '%.50s' at %p>",
                      self, Py_TYPE(ob)->tp_name, ob, name);
        Py_XDECREF(nameobj);
        Py_DECREF(ob);
    }

    return PyUnicode_FromString(buffer);
}

/* Weak references only support equality, not ordering. Two weak references
   are equal if the underlying objects are equal. If the underlying object has
   gone away, they are equal if they are identical. */

static PyObject *
weakref_richcompare(PyWeakReference *self, PyWeakReference *other, int op)
{
    PyObject *self_ob, *other_ob;
    PyObject *res;

    if ((op != Py_EQ && op != Py_NE) ||
            !PyWeakref_Check(self) ||
            !PyWeakref_Check(other)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    if (self == other) {
        Py_INCREF(Py_True);
        return Py_True;
    }

#warning XXX FIXME weakref comparisons should probably just use identity
    self_ob = PyWeakref_GetObjectEx((PyObject *)self);
    other_ob = PyWeakref_GetObjectEx((PyObject *)other);

    if (self_ob == NULL || other_ob == NULL) {
        res = Py_False;
        Py_INCREF(res);
    } else
        res = PyObject_RichCompare(self_ob, other_ob, op);

    Py_XDECREF(self_ob);
    Py_XDECREF(other_ob);
    return res;
}

static int
parse_weakref_init_args(char *funcname, PyObject *args, PyObject *kwargs,
                        PyObject **obp)
{
    /* XXX Should check that kwargs == NULL or is empty. */
    return PyArg_UnpackTuple(args, funcname, 1, 1, obp);
}

static PyObject *
weakref___new__(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *ob;

    if (!parse_weakref_init_args("__new__", args, kwargs, &ob))
        return NULL;

    return PyWeakref_NewRef(ob, NULL);
}

static int
weakref___init__(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ob;

    if (parse_weakref_init_args("__init__", args, kwargs, &ob))
        return 0;
    else
        return 1;
}


PyTypeObject
_PyWeakref_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "weakref",
    sizeof(PyWeakReference),
    0,
    (destructor)weakref_dealloc,  /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare*/
    (reprfunc)weakref_repr,     /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash*/
    weakref_call,               /*tp_call*/
    0,                          /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    /* There is only ever one weakref per object, so subclassing is
     * unsupported. */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_SHAREABLE,  /*tp_flags*/
    0,                          /*tp_doc*/
    (traverseproc)weakref_traverse,  /*tp_traverse*/
    0,                          /*tp_clear*/
    (richcmpfunc)weakref_richcompare,	/*tp_richcompare*/
    0,                          /*tp_weaklistoffset*/
    0,                          /*tp_iter*/
    0,                          /*tp_iternext*/
    0,                          /*tp_methods*/
    0,                          /*tp_members*/
    0,                          /*tp_getset*/
    0,                          /*tp_base*/
    0,                          /*tp_dict*/
    0,                          /*tp_descr_get*/
    0,                          /*tp_descr_set*/
    0,                          /*tp_dictoffset*/
    weakref___init__,           /*tp_init*/
    weakref___new__,            /*tp_new*/
};


static void
deathqueuehandle_dealloc(PyDeathQueueHandle *self)
{
    /* queue holds a reference to us, so we should never get deleted
     * without clearing us first. */
    assert(self->payload == NULL);
    assert(self->weakref == NULL);
    assert(self->queue == NULL);
    assert(PyLinkedList_Detached(&self->weakref_links));
    assert(PyLinkedList_Detached(&self->queue_links));
    PyCritical_Free(self->crit);
    PyObject_Del(self);
}

static PyObject *
deathqueuehandle_repr(PyDeathQueueHandle *self)
{
    char buffer[256];
    PyObject *payload;
    PyWeakReference *weakref;

    PyCritical_Enter(self->crit);
    payload = self->payload;
    weakref = self->weakref;
    Py_XINCREF(payload);
    Py_XINCREF(weakref);
    PyCritical_Exit(self->crit);

    if (payload == NULL)
        PyOS_snprintf(buffer, sizeof(buffer),
            "<deathqueuehandle at %p; cancelled/processed>", self);
    else {
        char *name = NULL;
        char *state = (weakref != NULL) ? "live" : "dead";
        PyObject *nameobj = PyObject_GetAttrString(payload, "__name__");

        if (nameobj == NULL)
            PyErr_Clear();
        else if (PyUnicode_Check(nameobj))
            name = PyUnicode_AsString(nameobj);

        if (name != NULL)
            PyOS_snprintf(buffer, sizeof(buffer),
                "<deathqueuehandle at %p; payload '%.50s' at %p (%s); %s>",
                self, Py_TYPE(payload)->tp_name, payload, name, state);
        else
            PyOS_snprintf(buffer, sizeof(buffer),
                "<deathqueuehandle at %p; payload '%.50s' at %p; %s>",
                self, Py_TYPE(payload)->tp_name, payload, state);
        Py_XDECREF(nameobj);
    }

    Py_XDECREF(payload);
    Py_XDECREF(weakref);

    return PyUnicode_FromString(buffer);
}

static int
deathqueuehandle_traverse(PyDeathQueueHandle *self, visitproc visit, void *arg)
{
    Py_VISIT(self->payload);
    Py_VISIT(self->weakref);
    return 0;
}


PyTypeObject
_PyDeathQueueHandle_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "deathqueuehandle",
    sizeof(PyDeathQueueHandle),
    0,
    (destructor)deathqueuehandle_dealloc,  /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare*/
    (reprfunc)deathqueuehandle_repr,  /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash*/
    0,                          /*tp_call*/
    0,                          /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_SHAREABLE,  /*tp_flags*/
    0,                          /*tp_doc*/
    (traverseproc)deathqueuehandle_traverse,  /*tp_traverse*/
    0,                          /*tp_clear*/
    0,                          /*tp_richcompare*/
    0,                          /*tp_weaklistoffset*/
    0,                          /*tp_iter*/
    0,                          /*tp_iternext*/
    0,                          /*tp_methods*/
    0,                          /*tp_members*/
    0,                          /*tp_getset*/
    0,                          /*tp_base*/
    0,                          /*tp_dict*/
    0,                          /*tp_descr_get*/
    0,                          /*tp_descr_set*/
    0,                          /*tp_dictoffset*/
    0,                          /*tp_init*/
    0,                          /*tp_new*/
};


static void
deathqueue_dealloc(PyDeathQueue *self)
{
    deathqueue_clear(self);

    if (Py_RefcntSnoop(self) != 1) {
        /* Another thread is trying to manipulate us.  Probably a
         * handle getting set to dead.  We can finish deleting later. */
        PyObject_Revive(self);
        Py_DECREF_ASYNC(self);
    } else {
        assert(PyLinkedList_Empty(&self->live_links));
        assert(PyLinkedList_Empty(&self->dead_links));
        PyCritical_Free(self->crit);
        PyThread_cond_free(self->cond);
        PyObject_Del(self);
    }
}

static int
deathqueue_traverse(PyDeathQueue *queue, visitproc visit, void *arg)
{
    PyDeathQueueHandle *handle = NULL;

    while (PyLinkedList_Next(&queue->live_links, &handle))
        Py_VISIT(handle);

    assert(handle == NULL);
    while (PyLinkedList_Next(&queue->dead_links, &handle))
        Py_VISIT(handle);

    return 0;
}

static int
deathqueue_clear(PyDeathQueue *queue)
{
    /* When called by the tracing GC (not deathqueue_dealloc), we don't
     * need to deal with the critical sections, as the tracing GC won't
     * run while a thread is in one.  It won't hurt either though. */
    while (1) {
        PyDeathQueueHandle *handle;

        PyCritical_Enter(queue->crit);
        handle = PyLinkedList_First(&queue->live_links);
        Py_XINCREF(handle);
        PyCritical_Exit(queue->crit);

        if (handle != NULL) {
            if (PyDeathQueue_Cancel(queue, handle))
                Py_FatalError("deathqueue_clear failed when calling "
                    "PyDeathQueue_Cancel");
            Py_DECREF(handle);
        } else
            break;
    }

    while (1) {
        PyDeathQueueHandle *handle;

        PyCritical_Enter(queue->crit);
        handle = PyLinkedList_First(&queue->dead_links);
        Py_XINCREF(handle);
        PyCritical_Exit(queue->crit);

        if (handle != NULL) {
            if (PyDeathQueue_Cancel(queue, handle))
                Py_FatalError("deathqueue_clear failed when calling "
                    "PyDeathQueue_Cancel");
            Py_DECREF(handle);
        } else
            break;
    }

    return 0;
}

static PyObject *
deathqueue___new__(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyDeathQueue *queue;

    static char *kwlist[] = {NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "", kwlist))
        return NULL;

    queue = PyObject_New(&_PyDeathQueue_Type);
    if (queue == NULL)
        return NULL;

    queue->crit = PyCritical_Allocate(PyCRITICAL_WEAKREF_QUEUE);
    if (queue->crit == NULL) {
        PyObject_Del(queue);
        PyErr_NoMemory();
        return NULL;
    }

    queue->cond = PyThread_cond_allocate();
    if (queue->cond == NULL) {
        PyCritical_Free(queue->crit);
        PyObject_Del(queue);
        PyErr_NoMemory();
        return NULL;
    }

    PyLinkedList_InitBase(&queue->live_links,
        offsetof(PyDeathQueueHandle, queue_links));
    PyLinkedList_InitBase(&queue->dead_links,
        offsetof(PyDeathQueueHandle, queue_links));

    return (PyObject *)queue;
}

static int
deathqueue___init__(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "", kwlist))
        return 1;

    return 0;
}

static PyObject *
deathqueue_watch(PyDeathQueue *queue, PyObject *args)
{
    PyObject *obj, *payload;
    PyWeakReference *ref;
    PyDeathQueueHandle *handle;

    if (!PyArg_UnpackTuple(args, "watch", 2, 2, &obj, &payload))
        return NULL;

    if (!PyObject_IsShareable(payload)) {
        PyErr_Format(PyExc_TypeError,
            "deathqueue.watch()'s payload argument must be shareable, '%s' object "
            "is not", payload->ob_type->tp_name);
        return NULL;
    }

    ref = (PyWeakReference *)PyWeakref_NewRef(obj, NULL);
    if (ref == NULL)
        return NULL;

    handle = PyObject_New(&_PyDeathQueueHandle_Type);
    if (handle == NULL) {
        Py_DECREF(ref);
        return NULL;
    }

    handle->crit = PyCritical_Allocate(PyCRITICAL_WEAKREF_HANDLE);
    if (handle->crit == NULL) {
        PyObject_Del(handle);
        Py_DECREF(ref);
        return NULL;
    }

    PyCritical_Enter(ref->crit);
    /* We skip handle->crit as nobody else has a reference to handle yet. */
    PyCritical_Enter(queue->crit);

    assert(ref->wr_object != NULL);

    /* The underlying ownership order is queue -> handle -> weakref.
     * queue needs a reference to us, but handle doesn't INCREF them
     * (they clear our pointer if they get deleted), and we already have
     * a reference to weakref. */
    Py_INCREF(handle);

    Py_INCREF(payload);
    handle->payload = payload;
    handle->queue = queue;
    handle->weakref = ref;
    PyLinkedList_InitNode(&handle->weakref_links);
    PyLinkedList_InitNode(&handle->queue_links);

    PyLinkedList_Append(&ref->handle_links, handle);
    PyLinkedList_Append(&queue->live_links, handle);

    PyCritical_Exit(queue->crit);
    PyCritical_Exit(ref->crit);

    return (PyObject *)handle;
}

PyObject *
deathqueue_cancel(PyDeathQueue *queue, PyObject *args)
{
    PyDeathQueueHandle *handle;

    if (!PyArg_UnpackTuple(args, "cancel", 1, 1, &handle))
        return NULL;

    if (!PyDeathQueueHandle_Check(handle)) {
        PyErr_Format(PyExc_TypeError, "cancel expected deathqueuehandle "
            "(not \"%.200s\")", Py_TYPE(handle)->tp_name);
        return NULL;
    }

    if (PyDeathQueue_Cancel(queue, handle))
        return NULL;

    Py_INCREF(Py_None);
    return Py_None;
}

/* Warning: this function deletes queue's reference to handle.  If the
 * caller doesn't have their own reference to handle, it may be gone by
 * the time this function returns. */
static int
PyDeathQueue_Cancel(PyDeathQueue *queue, PyDeathQueueHandle *handle)
{
    PyWeakReference *ref;
    int decref_weakref = 0;
    PyObject *payload;

    /* "climb" up the ordered critical sections */
    PyCritical_Enter(handle->crit);
    ref = handle->weakref;
    Py_XINCREF(ref);
    PyCritical_Exit(handle->crit);

    /* Begin entering all 3 critical sections (weakref, handle, queue) */
    if (ref != NULL)
        PyCritical_Enter(ref->crit);
    PyCritical_Enter(handle->crit);

    /* Early out if we've got nothing to do */
    if (handle->queue != queue) {
        PyDeathQueue *badqueue = handle->queue;
        PyCritical_Exit(handle->crit);
        if (ref != NULL)
            PyCritical_Exit(ref->crit);
        Py_XDECREF(ref);

        if (handle->queue == NULL) {
            /* Already cleared */
            return 0;
        } else {
            /* Wrong queue! */
            PyErr_Format(PyExc_ValueError, "cancel called on %p queue "
                "but handle %p is for %p queue", queue, handle, badqueue);
            return 1;
        }
    }

    PyCritical_Enter(queue->crit);

    /* The real work of this function */
    if (handle->weakref != NULL) {
        PyLinkedList_Remove(&handle->weakref_links);
        handle->weakref = NULL;
        decref_weakref = 1;
    }
    payload = handle->payload;
    handle->payload = NULL;

    PyLinkedList_Remove(&handle->queue_links);
    handle->queue = NULL;

    /* Exit the critical sections */
    PyCritical_Exit(queue->crit);
    PyCritical_Exit(handle->crit);
    if (ref != NULL)
        PyCritical_Exit(ref->crit);

    /* Finally, cleanup */
    Py_DECREF(handle);  /* queue -> handle reference */
    Py_XDECREF(ref);  /* our reference */
    if (decref_weakref)
        Py_DECREF(ref);  /* handle -> weakref reference */
    Py_XDECREF(payload);

    return 0;
}

/* Returns 0 for success and 1 for empty */
static int
pop_common(PyDeathQueue *queue, PyObject **payload)
{
    PyDeathQueueHandle *handle;

    while (1) {
        PyCritical_Enter(queue->crit);
        handle = PyLinkedList_First(&queue->dead_links);
        Py_XINCREF(handle);
        PyCritical_Exit(queue->crit);

        if (handle == NULL)
            return 1;

        PyCritical_Enter(handle->crit);
        PyCritical_Enter(queue->crit);

        if (handle->queue == NULL) {
            /* Another thread popped the handle on us, or cancelled it */
            PyCritical_Exit(queue->crit);
            PyCritical_Exit(handle->crit);
            Py_DECREF(handle);
            continue;
        }

        assert(handle->weakref == NULL);
        PyLinkedList_Remove(&handle->queue_links);
        *payload = handle->payload;
        handle->payload = NULL;  /* We steal their reference */
        handle->queue = NULL;

        PyCritical_Exit(queue->crit);
        PyCritical_Exit(handle->crit);

        Py_DECREF(handle);
        return 0;
    }
}

static PyObject *
deathqueue_pop(PyDeathQueue *queue)
{
    PyObject *payload, *x;

    while (1) {
        x = deathqueue_wait(queue);
        if (x == NULL)
            return NULL;
        Py_DECREF(x);
        if (!pop_common(queue, &payload))
            return payload;
    }
}

static PyObject *
deathqueue_trypop(PyDeathQueue *queue)
{
    PyObject *payload;

    if (pop_common(queue, &payload)) {
        PyErr_SetString(PyExc_ValueError, "trypop from empty deathqueue");
        return NULL;
    } else
        return payload;
}

static int
deathqueue_bool(PyDeathQueue *queue)
{
    int result;

    PyCritical_Enter(queue->crit);
    result = !PyLinkedList_Empty(&queue->dead_links);
    PyCritical_Exit(queue->crit);

    return result;
}

static PyObject *
deathqueue_wait(PyDeathQueue *queue)
{
    /* Using this method is racey, giving you extra wakeups and forcing
     * you to have retry loops, so we don't bother to add another retry
     * loop inside it. */
    PyCritical_Enter(queue->crit);

    if (PyLinkedList_Empty(&queue->dead_links))
        _PyCritical_CondWait(queue->crit, queue->cond);

    PyCritical_Exit(queue->crit);
    Py_INCREF(Py_None);
    return Py_None;
}


PyDoc_STRVAR(watch_doc,
"deathqueue.watch(obj, payload) -> handle.  payload is returned from\n\
deathqueue.pop() once obj dies, unless canceled first.");

PyDoc_STRVAR(cancel_doc,
"deathqueue.cancel(handle) -> None.  Cancels watching of associated obj.");

PyDoc_STRVAR(pop_doc,
"deathqueue.pop() -> payload.  Returns the payload passed to\n\
deathqueue.watch().  Blocks if no watched objects have died yet.");

PyDoc_STRVAR(trypop_doc,
"deathqueue.trypop() -> payload.  Used once bool(deathqueue) or\n\
deathqueue.wait() indicate a watched obj has died; returns the payload\n\
passed to deathqueue.watch().");

PyDoc_STRVAR(wait_doc,
"deathqueue.wait() -> None.  Does not return until a watched obj has\n\
died.  This function is cancellable.");

static PyNumberMethods deathqueue_as_number = {
    0,                              /*nb_add*/
    0,                              /*nb_subtract*/
    0,                              /*nb_multiply*/
    0,                              /*nb_remainder*/
    0,                              /*nb_divmod*/
    0,                              /*nb_power*/
    0,                              /*nb_negative*/
    0,                              /*nb_positive*/
    0,                              /*nb_absolute*/
    (inquiry)deathqueue_bool,       /*nb_bool*/
};

static PyMethodDef deathqueue_methods[] = {
    {"watch",       (PyCFunction)deathqueue_watch,      METH_VARARGS, watch_doc},
    {"cancel",      (PyCFunction)deathqueue_cancel,     METH_VARARGS, cancel_doc},
    {"pop",         (PyCFunction)deathqueue_pop,        METH_NOARGS, pop_doc},
    {"trypop",      (PyCFunction)deathqueue_trypop,     METH_NOARGS, trypop_doc},
    {"wait",        (PyCFunction)deathqueue_wait,       METH_NOARGS, wait_doc},
    {NULL}
};

static int
deathqueue_isshareable (PyDeathQueue *queue)
{
    return 1;
}

PyTypeObject
_PyDeathQueue_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "deathqueue",
    sizeof(PyDeathQueue),
    0,
    (destructor)deathqueue_dealloc,  /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare*/
    0,                          /*tp_repr*/
    &deathqueue_as_number,      /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash*/
    0,                          /*tp_call*/
    0,                          /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_SHAREABLE,  /*tp_flags*/
    0,                          /*tp_doc*/
    (traverseproc)deathqueue_traverse,  /*tp_traverse*/
    0,                          /*tp_clear*/
    0,                          /*tp_richcompare*/
    0,                          /*tp_weaklistoffset*/
    0,                          /*tp_iter*/
    0,                          /*tp_iternext*/
    deathqueue_methods,         /*tp_methods*/
    0,                          /*tp_members*/
    0,                          /*tp_getset*/
    0,                          /*tp_base*/
    0,                          /*tp_dict*/
    0,                          /*tp_descr_get*/
    0,                          /*tp_descr_set*/
    0,                          /*tp_dictoffset*/
    deathqueue___init__,        /*tp_init*/
    deathqueue___new__,         /*tp_new*/
    0,                          /*tp_is_gc*/
    0,                          /*tp_bases*/
    0,                          /*tp_mro*/
    0,                          /*tp_cache*/
    0,                          /*tp_subclasses*/
    0,                          /*tp_weaklist*/
    (isshareablefunc)deathqueue_isshareable,    /*tp_isshareable*/
};


PyObject *
PyWeakref_NewRef(PyObject *ob, PyObject *callback)
{
    PyWeakReference **ptr;
    PyWeakReference *ref;

    if (!PyType_SUPPORTS_WEAKREFS(Py_TYPE(ob))) {
        PyErr_Format(PyExc_TypeError,
                     "cannot create weak reference to '%s' object",
                     Py_TYPE(ob)->tp_name);
        return NULL;
    }
    if (callback != NULL) {
        PyErr_Format(PyExc_TypeError,
            "weakrefs no longer support callbacks");
        return NULL;
    }

    ptr = _PY_GETWEAKREFPTR(ob);

    /* XXX FIXME We should have some sort of fake critical section to
     * ensure the tracing GC doesn't activate and delete the weakref */

    /* Use the existing ref if there is one */
    ref = (PyWeakReference *)AO_load_full((AO_t *)ptr);
    if (ref != NULL) {
        Py_INCREF(ref);
        return (PyObject *)ref;
    }

    /* If there isn't a ref we start creating one */
    ref = PyObject_New(&_PyWeakref_Type);
    ref->crit = PyCritical_Allocate(PyCRITICAL_WEAKREF_REF);
    if (ref->crit == NULL) {
        PyObject_Del(ref);
        PyErr_NoMemory();
        return NULL;
    }
    ref->hash = (AO_t)-1;
    ref->wr_object = ob;
    PyLinkedList_InitBase(&ref->handle_links,
        offsetof(PyDeathQueueHandle, weakref_links));
    PyLinkedList_InitBase(&ref->binding_links,
        offsetof(PyWeakBinding, weakref_links));

    if (!AO_compare_and_swap_full((AO_t *)ptr, (AO_t)NULL, (AO_t)ref)) {
        /* Another thread beat us to it.  Use theirs instead. */
        Py_DECREF(ref);
        ref = (PyWeakReference *)AO_load_full((AO_t *)ptr);
        assert(ref != NULL);
    }

    /* ob has the original reference, so we need another one to return
     * to our caller */
    Py_INCREF(ref);
    return (PyObject *)ref;
}


/* Unlike the old function, this DOES include an INCREF */
PyObject *
PyWeakref_GetObjectEx(PyObject *ref_)
{
    PyObject *ob;
    PyWeakReference *ref = (PyWeakReference *)ref_;

    if (ref == NULL || !PyWeakref_Check(ref)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    PyCritical_Enter(ref->crit);
    ob = ref->wr_object;
    Py_XINCREF(ob);
    PyCritical_Exit(ref->crit);

    return ob;
}


static void
_PyWeakref_delete_common(PyObject *object, PyWeakReference *ref)
{
    PyWeakReference **ptr = _PY_GETWEAKREFPTR(object);
    ref->wr_object = NULL;
    *ptr = NULL;
    Py_DECREF(ref);

    while (!PyLinkedList_Empty(&ref->handle_links)) {
        PyDeathQueueHandle *handle = PyLinkedList_First(&ref->handle_links);

        PyCritical_Enter(handle->crit);
        assert(handle->queue != NULL);
        PyCritical_Enter(handle->queue->crit);

        PyLinkedList_Remove(&handle->weakref_links);
        handle->weakref = NULL;
        /* There should always be one reference remaining, borrowed
         * from the caller. */
        Py_DECREF(ref);

        PyLinkedList_Remove(&handle->queue_links);
        PyLinkedList_Append(&handle->queue->dead_links, handle);
        PyThread_cond_wakeall(handle->queue->cond);

        PyCritical_Exit(handle->queue->crit);
        PyCritical_Exit(handle->crit);
    }
}

static int
_PyWeakref_TryDelete(PyObject *object, PyWeakReference *ref)
{
    PyCritical_Enter(ref->crit);
    if (ref->wr_object == NULL) {
        PyCritical_Exit(ref->crit);
        return 0;
    }
    assert(ref->wr_object == object);

    if (Py_RefcntSnoop(object) != 1) {
        /* Brought back from the brink of death! */
        PyCritical_Exit(ref->crit);
        return 1;
    } else {
        _PyWeakref_delete_common(object, ref);

        PyCritical_Exit(ref->crit);

        _PyWeakref_ClearBindings(object, ref);

        return 0;
    }
}

/* XXX This version will actually be called by Py_Dealloc and may
 * indicate the object is not to be deleted after all. */
/* XXX FIXME this whole function should get moved into gcmodule.c */
int
_PyObject_TryClearWeakref(PyObject *object)
{
    PyWeakReference **ptr;
    PyWeakReference *ref;
    int result;

    if (object == NULL ||
            !PyType_SUPPORTS_WEAKREFS(Py_TYPE(object)) ||
            !Py_RefcntMatches(object, 1)) {
        PyErr_BadInternalCall();
        return 0;
    }
    ptr = _PY_GETWEAKREFPTR(object);

    ref = (PyWeakReference *)AO_load_full((AO_t *)ptr);
    if (ref == NULL)
        return 0;
    Py_INCREF(ref);

    result = _PyWeakref_TryDelete(object, ref);

    Py_DECREF(ref);
    return result;
}

void
_PyObject_ForceClearWeakref(PyObject *object)
{
    PyWeakReference *ref;
    PyWeakReference **ptr = _PY_GETWEAKREFPTR(object);

    ref = (PyWeakReference *)AO_load_full((AO_t *)ptr);
    if (ref == NULL)
        return;

    Py_INCREF(ref);

    PyCritical_Enter(ref->crit);
    assert(ref->wr_object == object);
    _PyWeakref_delete_common(object, ref);
    PyCritical_Exit(ref->crit);

    _PyWeakref_ClearBindings(object, ref);

    Py_DECREF(ref);
}

PyObject *
PyWeakref_NewBinding(PyObject *ob, PyObject *value)
{
    PyWeakReference *ref;
    PyWeakBinding *bind;

    ref = (PyWeakReference *)PyWeakref_NewRef(ob, NULL);
    if (ref == NULL)
        return NULL;

    bind = PyObject_New(&_PyWeakBinding_Type);
    if (bind == NULL) {
        Py_DECREF(ref);
        return NULL;
    }

    PyCritical_Enter(ref->crit);
    assert(ref->wr_object != NULL);
    bind->weakref = ref;
    Py_INCREF(value); /* This is actually owned by ob */
    bind->value = value;
    PyLinkedList_InitNode(&bind->weakref_links);
    PyLinkedList_Append(&ref->binding_links, bind);
    PyCritical_Exit(ref->crit);

    return (PyObject *)bind;
}

PyObject *
PyWeakref_GetBindingObject(PyObject *bind_, PyObject **value)
{
    PyObject *ob;
    PyWeakBinding *bind = (PyWeakBinding *)bind_;

    if (bind == NULL || !PyWeakBinding_Check(bind) || value == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }

    PyCritical_Enter(bind->weakref->crit);
    if (!PyLinkedList_Detached(&bind->weakref_links) &&
            bind->weakref->wr_object != NULL) {
        ob = bind->weakref->wr_object;
        Py_INCREF(ob);
        *value = bind->value;
        Py_INCREF(*value);
        PyCritical_Exit(bind->weakref->crit);
        return ob;
    } else {
        PyCritical_Exit(bind->weakref->crit);
        *value = NULL;
        return NULL;
    }
}

void
_PyWeakref_ClearBindings(PyObject *ob, PyWeakReference *ref)
{
    /* Deleting the values may cause another binding to be deleted,
     * reentering the critical section.  Thus the hoops about making
     * sure all the bindings we want stay alive long enough, and exiting
     * the critical section while calling DECREF */
    PyWeakBinding *bind = NULL;
    PyObject *value;

    PyCritical_Enter(ref->crit);
    assert(ref->wr_object == NULL);

    while (PyLinkedList_Next(&ref->binding_links, &bind))
        Py_INCREF(bind);

    while (!PyLinkedList_Empty(&ref->binding_links)) {
        bind = PyLinkedList_First(&ref->binding_links);

        PyLinkedList_Remove(&bind->weakref_links);
        value = bind->value;
        bind->value = NULL;

        PyCritical_Exit(ref->crit);
        Py_DECREF(bind);
        Py_DECREF(value);
        PyCritical_Enter(ref->crit);
    }

    PyCritical_Exit(ref->crit);
}

static void
weakbind_dealloc(PyWeakBinding *bind)
{
    PyCritical_Enter(bind->weakref->crit);
    if (!PyLinkedList_Detached(&bind->weakref_links)) {
        PyObject *value;
        assert(bind->value);
        PyLinkedList_Remove(&bind->weakref_links);
        value = bind->value;
        bind->value = NULL;
        /* At this point we've taken ob's reference to value and now
         * own it directly. */
        PyCritical_Exit(bind->weakref->crit);
        Py_DECREF(value);
    } else
        PyCritical_Exit(bind->weakref->crit);

    if (Py_RefcntSnoop(bind) != 1) {
        /* Another thread is trying to manipulate us.  Probably a
         * clearing all the bindings for a weakref.  We can finish
         * deleting later. */
        PyObject_Revive(bind);
        Py_DECREF_ASYNC(bind);
        return;
    }

    Py_DECREF(bind->weakref);

    PyObject_Del(bind);
}

static int
weakbind_traverse(PyWeakBinding *bind, visitproc visit, void *arg)
{
    Py_VISIT(bind->weakref);
    return 0;
}

static PyObject *
weakbind_call(PyObject *bind, PyObject *args, PyObject *kw)
{
    static char *kwlist[] = {NULL};
    PyObject *ob, *value;

    if (!PyArg_ParseTupleAndKeywords(args, kw, ":__call__", kwlist))
        return NULL;

    ob = PyWeakref_GetBindingObject(bind, &value);

    if (ob == NULL)
        return Py_BuildValue("OO", Py_None, Py_None);
    else
        return Py_BuildValue("NN", ob, value);
}

static PyObject *
weakbind_repr(PyWeakBinding *bind)
{
    char buffer[256];
    PyObject *ob, *value;

    ob = PyWeakref_GetBindingObject((PyObject *)bind, &value);

    if (ob == NULL)
        PyOS_snprintf(buffer, sizeof(buffer), "<weakbinding at %p; dead>", bind);
    else {
        PyOS_snprintf(buffer, sizeof(buffer),
            "<weakbinding at %p; from '%.50s' at %p to '%.50s' at %p>",
            bind, Py_TYPE(ob)->tp_name, ob, Py_TYPE(value)->tp_name, value);
        Py_DECREF(ob);
        Py_DECREF(value);
    }

    return PyUnicode_FromString(buffer);
}

static int
parse_weakbind_init_args(char *funcname, PyObject *args, PyObject *kwargs,
                        PyObject **obp, PyObject **valuep)
{
    /* XXX Should check that kwargs == NULL or is empty. */
    return PyArg_UnpackTuple(args, funcname, 2, 2, obp, valuep);
}

static PyObject *
weakbind___new__(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *ob, *value;

    if (!parse_weakbind_init_args("__new__", args, kwargs, &ob, &value))
        return NULL;

    return PyWeakref_NewBinding(ob, value);
}

static int
weakbind___init__(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ob, *value;

    if (parse_weakbind_init_args("__init__", args, kwargs, &ob, &value))
        return 0;
    else
        return 1;
}


PyTypeObject
_PyWeakBinding_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "weakbinding",
    sizeof(PyWeakBinding),
    0,
    (destructor)weakbind_dealloc,  /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare*/
    (reprfunc)weakbind_repr,    /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash*/
    weakbind_call,              /*tp_call*/
    0,                          /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    /* subclassing is unsupported */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_SHAREABLE,  /*tp_flags*/
    0,                          /*tp_doc*/
    (traverseproc)weakbind_traverse,  /*tp_traverse*/
    0,                          /*tp_clear*/
    0,                          /*tp_richcompare*/
    0,                          /*tp_weaklistoffset*/
    0,                          /*tp_iter*/
    0,                          /*tp_iternext*/
    0,                          /*tp_methods*/
    0,                          /*tp_members*/
    0,                          /*tp_getset*/
    0,                          /*tp_base*/
    0,                          /*tp_dict*/
    0,                          /*tp_descr_get*/
    0,                          /*tp_descr_set*/
    0,                          /*tp_dictoffset*/
    weakbind___init__,          /*tp_init*/
    weakbind___new__,           /*tp_new*/
};
