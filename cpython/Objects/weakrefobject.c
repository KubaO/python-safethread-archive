#include "Python.h"
#include "structmember.h"


#define GET_WEAKREFPTR(o) \
        ((PyWeakReference **) (((char *) (o)) + Py_Type(o)->tp_weaklistoffset))

static int deathqueue_clear(PyDeathQueue *self);
static int PyDeathQueue_Cancel(PyDeathQueue *queue, PyDeathQueueHandle *handle);


static void
weakref_dealloc(PyWeakReference *self)
{
    PyObject_GC_UnTrack(self);
    if (self->wr_object != NULL)
        Py_FatalError("Still-valid weakref deleted!");
    PyCritical_Free(self->crit);
    PyObject_DEL(self);
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

static long
weakref_hash(PyWeakReference *self)
{
    PyObject *ob;
    AO_t hash = AO_load_full(&self->hash);
    if (hash != (AO_t)-1)
        return (long)hash;

    ob = PyWeakref_GetObjectEx((PyObject *)self);
    if (ob == NULL) {
        PyErr_SetString(PyExc_TypeError, "weak object has gone away");
        return -1;
    }

    hash = PyObject_Hash(ob);
    Py_DECREF(ob);

    AO_compare_and_swap(&self->hash, (AO_t)-1, hash);
    return (long)AO_load_full(&self->hash);
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
                      self, Py_Type(ob)->tp_name, ob, name);
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
    return PyArg_UnpackTuple(args, funcname, 1, 2, obp);
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
    (hashfunc)weakref_hash,     /*tp_hash*/
    weakref_call,               /*tp_call*/
    0,                          /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    /* There is only ever one weakref per object, so subclassing is
     * unsupported. */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,  /*tp_flags*/
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
    PyObject_GC_UnTrack(self);

    /* queue holds a reference to us, so we should never get deleted
     * without clearing us first. */
    assert(self->weakref == NULL);
    assert(self->queue == NULL);
    assert(PyLinkedList_Detatched(&self->weakref_links));
    assert(PyLinkedList_Detatched(&self->queue_links));
    PyCritical_Free(self->crit);
    PyObject_DEL(self);
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
                self, Py_Type(payload)->tp_name, payload, name, state);
        else
            PyOS_snprintf(buffer, sizeof(buffer),
                "<deathqueuehandle at %p; payload '%.50s' at %p; %s>",
                self, Py_Type(payload)->tp_name, payload, state);
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
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,  /*tp_flags*/
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
    PyObject_GC_UnTrack(self);

    deathqueue_clear(self);

    if (Py_RefcntSnoop(self) != 1) {
        /* Another thread is trying to manipulate us.  Probably a
         * handle getting set to dead.  We can finish deleting later. */
        Py_DECREF_ASYNC(self);
    } else {
        assert(PyLinkedList_Empty(&self->live_links));
        assert(PyLinkedList_Empty(&self->dead_links));
        PyCritical_Free(self->crit);
        PyObject_DEL(self);
    }
}

static int
deathqueue_traverse(PyDeathQueue *queue, visitproc visit, void *arg)
{
    PyDeathQueueHandle *handle;
    PyLinkedList *handle_links;

    handle_links = &queue->live_links;
    while (PyLinkedList_Next(&queue->live_links, &handle_links)) {
        handle = PyLinkedList_Restore(PyDeathQueueHandle, queue_links,
            handle_links);
        Py_VISIT(handle);
    }

    handle_links = &queue->dead_links;
    while (PyLinkedList_Next(&queue->dead_links, &handle_links)) {
        handle = PyLinkedList_Restore(PyDeathQueueHandle, queue_links,
            handle_links);
        Py_VISIT(handle);
    }

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
        if (!PyLinkedList_Empty(&queue->live_links)) {
            handle = PyLinkedList_Restore(PyDeathQueueHandle,
                queue_links, queue->live_links.next);
            Py_INCREF(handle);
        } else
            handle = NULL;
        PyCritical_Exit(queue->crit);

        if (handle != NULL) {
            if (PyDeathQueue_Cancel(queue, handle))
                Py_FatalError("deathqueue_clear failed when calling "
                    "PyDeathQueue_Cancel");
        } else
            break;
    }

    while (1) {
        PyDeathQueueHandle *handle;

        PyCritical_Enter(queue->crit);
        if (!PyLinkedList_Empty(&queue->dead_links)) {
            handle = PyLinkedList_Restore(PyDeathQueueHandle,
                queue_links, queue->dead_links.next);
            Py_INCREF(handle);
        } else
            handle = NULL;
        PyCritical_Exit(queue->crit);

        if (handle != NULL) {
            if (PyDeathQueue_Cancel(queue, handle))
                Py_FatalError("deathqueue_clear failed when calling "
                    "PyDeathQueue_Cancel");
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

    queue = PyObject_NEW(PyDeathQueue, &_PyDeathQueue_Type);
    if (queue == NULL)
        return NULL;

    queue->crit = PyCritical_Allocate(PyCRITICAL_WEAKREF_QUEUE);
    if (queue->crit == NULL) {
        PyObject_DEL(queue);
        PyErr_NoMemory();
        return NULL;
    }

    queue->live_links.prev = &queue->live_links;
    queue->live_links.next = &queue->live_links;
    queue->dead_links.prev = &queue->dead_links;
    queue->dead_links.next = &queue->dead_links;

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

    ref = (PyWeakReference *)PyWeakref_NewRef(obj, NULL);
    if (ref == NULL)
        return NULL;

    handle = PyObject_NEW(PyDeathQueueHandle, &_PyDeathQueueHandle_Type);
    if (handle == NULL) {
        Py_DECREF(ref);
        return NULL;
    }

    handle->crit = PyCritical_Allocate(PyCRITICAL_WEAKREF_HANDLE);
    if (handle->crit == NULL) {
        PyObject_DEL(handle);
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

    PyLinkedList_Append(&ref->handles, &handle->weakref_links);
    PyLinkedList_Append(&queue->live_links, &handle->queue_links);

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
            "(not \"%.200s\")", Py_Type(handle)->tp_name);
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

    PyLinkedList_Remove(&handle->queue_links);
    handle->queue = NULL;
    /* XXX FIXME do whatever's necessary for deathqueue_wait */

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

    return 0;
}

static PyObject *
deathqueue_pop(PyDeathQueue *queue)
{
    PyDeathQueueHandle *handle;
    PyObject *payload;

    while (1) {
        PyCritical_Enter(queue->crit);
        if (PyLinkedList_Empty(&queue->dead_links)) {
            PyCritical_Exit(queue->crit);
            PyErr_SetString(PyExc_ValueError, "pop from empty deathqueue");
            return NULL;
        }
        handle = PyLinkedList_Restore(PyDeathQueueHandle, queue_links,
            queue->dead_links.next);
        Py_INCREF(handle);
        PyCritical_Exit(queue->crit);

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
        payload = handle->payload;
        handle->payload = NULL;  /* We steal their reference */

        PyCritical_Exit(queue->crit);
        PyCritical_Exit(handle->crit);

        Py_DECREF(handle);
        return payload;
    }
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
    PyErr_SetString(PyExc_NotImplementedError, "deathqueue.wait() isn't "
        "implemented yet.");
    return NULL;
}


PyDoc_STRVAR(watch_doc,
"deathqueue.watch(obj, payload) -> handle.  payload is returned from\n\
deathqueue.pop() once obj dies, unless canceled first.");

PyDoc_STRVAR(cancel_doc,
"deathqueue.cancel(handle) -> None.  Cancels watching of associated obj.");

PyDoc_STRVAR(pop_doc,
"deathqueue.pop() -> payload.  Used once bool(deathqueue) or\n\
deathqueue.wait() indicate a watched obj has died; returns the payload\n\
passed to deathqueue.watch().");

PyDoc_STRVAR(wait_doc,
"deathqueue.wait() -> None.  Does not return until a watched obj has\n\
died.  This function is interruptible.");

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
    {"wait",        (PyCFunction)deathqueue_wait,       METH_NOARGS, wait_doc},
    {NULL}
};

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
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,  /*tp_flags*/
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
};


PyObject *
PyWeakref_NewRef(PyObject *ob, PyObject *callback)
{
    PyWeakReference **ptr;
    PyWeakReference *ref;

    if (!PyType_SUPPORTS_WEAKREFS(Py_Type(ob))) {
        PyErr_Format(PyExc_TypeError,
                     "cannot create weak reference to '%s' object",
                     Py_Type(ob)->tp_name);
        return NULL;
    }
    if (callback != NULL) {
        PyErr_Format(PyExc_TypeError,
            "weakrefs no longer support callbacks");
        return NULL;
    }

    ptr = GET_WEAKREFPTR(ob);

    /* XXX FIXME We should have some sort of fake critical section to
     * ensure the tracing GC doesn't activate and delete the weakref */

    /* Use the existing ref if there is one */
    ref = (PyWeakReference *)AO_load_full((AO_t *)ptr);
    if (ref != NULL) {
        Py_INCREF(ref);
        return (PyObject *)ref;
    }

    /* If there isn't a ref we start creating one */
    ref = PyObject_NEW(PyWeakReference, &_PyWeakref_Type);
    ref->crit = PyCritical_Allocate(PyCRITICAL_WEAKREF_REF);
    if (ref->crit == NULL) {
        PyObject_DEL(ref);
        PyErr_NoMemory();
        return NULL;
    }
    ref->hash = (AO_t)-1;
    ref->wr_object = ob;
    ref->handles.prev = &ref->handles;
    ref->handles.next = &ref->handles;

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

int
_PyWeakref_TryDelete(PyObject *object, PyWeakReference *ref)
{
    PyCritical_Enter(ref->crit);
    assert(ref->wr_object == object);
    if (Py_RefcntSnoop(object) != 1) {
        /* Brought back from the brink of death! */
        PyCritical_Exit(ref->crit);
        return 1;
    } else {
        ref->wr_object = NULL;

        while (!PyLinkedList_Empty(&ref->handles)) {
            PyDeathQueueHandle *handle = PyLinkedList_Restore(
                PyDeathQueueHandle, weakref_links, ref->handles.next);

            PyCritical_Enter(handle->crit);
            assert(handle->queue != NULL);
            PyCritical_Enter(handle->queue->crit);

            PyLinkedList_Remove(&handle->weakref_links);
            handle->weakref = NULL;
            /* There should always be one reference remaining, borrowed
             * from the caller. */
            Py_DECREF(ref);

            PyLinkedList_Remove(&handle->queue_links);
            PyLinkedList_Append(&handle->queue->dead_links, &handle->queue_links);

            PyCritical_Exit(handle->queue->crit);
            PyCritical_Exit(handle->crit);
        }

        PyCritical_Exit(ref->crit);
        return 0;
    }
}

/* XXX This version will actually be called by Py_Dealloc and may
 * indicate the object is not to be deleted after all. */
/* XXX FIXME this whole function should get moved into gcmodule.c */
int
_PyObject_ClearWeakref(PyObject *object)
{
    PyWeakReference **ptr;
    PyWeakReference *ref;
    int result;

    if (object == NULL ||
            !PyType_SUPPORTS_WEAKREFS(Py_Type(object)) ||
            !Py_RefcntMatches(object, 1)) {
        PyErr_BadInternalCall();
        return 0;
    }
    ptr = GET_WEAKREFPTR(object);

    ref = (PyWeakReference *)AO_load_full((AO_t *)ptr);
    if (ref == NULL)
        return 0;
    Py_INCREF(ref);

    result = _PyWeakref_TryDelete(object, ref);

    Py_DECREF(ref);
    return result;
}
