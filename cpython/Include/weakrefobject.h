/* Weak references objects for Python. */

#ifndef Py_WEAKREFOBJECT_H
#define Py_WEAKREFOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif


#include "pythread.h"
#include "pylinkedlist.h"


typedef struct _PyWeakReference PyWeakReference;
typedef struct _PyDeathQueueHandle PyDeathQueueHandle;
typedef struct _PyDeathQueue PyDeathQueue;
typedef struct _PyWeakBinding PyWeakBinding;

struct _PyWeakReference {
    PyObject_HEAD

    /* Critical section protecting access to wr_object and our (not yet
     * implemented) death notice list.  hash is NOT protected */
    PyCritical *crit;

    /* The object to which this is a weak reference, or Py_None if none.
     * Note that this is a stealth reference:  wr_object's refcount is
     * not incremented to reflect this pointer.
     */
    /* XXX it gets set to NULL now, not Py_None */
    PyObject *wr_object;

    /* A cache for wr_object's hash code.  As usual for hashes, this is -1
     * if the hash code isn't known yet.
     */
    /* XXX I'd much rather remove this, but to detect existing usage
     * I'd have to make it an error. */
    AO_t hash;

    /* If wr_object is weakly referenced, wr_object has a doubly-linked NULL-
     * terminated list of weak references to it.  These are the list pointers.
     * If wr_object goes away, wr_object is set to Py_None, and these pointers
     * have no meaning then.
     */
    //PyWeakReference *wr_prev;
    //PyWeakReference *wr_next;
    PyLinkedList handle_links;

    PyLinkedList binding_links;
};

PyAPI_DATA(PyTypeObject) _PyWeakref_Type;

#define PyWeakref_Check(op) (Py_TYPE(op) == &_PyWeakref_Type)
#define PyWeakref_CheckRefExact PyWeakref_Check
#define PyWeakref_CheckRef PyWeakref_Check


PyAPI_FUNC(PyObject *) PyWeakref_NewRef(PyObject *ob, PyObject *callback);
/* Note that this DOES incref the returned object, if not NULL! */
PyAPI_FUNC(PyObject *) PyWeakref_GetObjectEx(PyObject *ref);
PyAPI_FUNC(PyObject *) PyWeakref_NewBinding(PyObject *ob, PyObject *value);
PyAPI_FUNC(PyObject *) PyWeakref_GetBindingObject(PyObject *bind, PyObject **value);

PyAPI_FUNC(void) _PyWeakref_ClearBindings(PyObject *ob, PyWeakReference *ref);

#define _PY_GETWEAKREFPTR(o) \
        ((PyWeakReference **) (((char *) (o)) + Py_TYPE(o)->tp_weaklistoffset))


struct _PyDeathQueueHandle {
    PyObject_HEAD

    PyCritical *crit;

    PyObject *payload;

    PyWeakReference *weakref;
    PyLinkedList weakref_links;

    PyDeathQueue *queue;
    PyLinkedList queue_links;
};

PyAPI_DATA(PyTypeObject) _PyDeathQueueHandle_Type;

#define PyDeathQueueHandle_Check(op) \
    (Py_TYPE(op) == &_PyDeathQueueHandle_Type)


struct _PyDeathQueue {
    PyObject_HEAD

    PyCritical *crit;
    PyThread_type_cond cond;

    PyLinkedList live_links;
    PyLinkedList dead_links;
};

PyAPI_DATA(PyTypeObject) _PyDeathQueue_Type;

#define PyDeathQueue_Check(op) (Py_TYPE(op) == &_PyDeathQueue_Type)


struct _PyWeakBinding {
    PyObject_HEAD

    PyWeakReference *weakref;
    PyObject *value; /* This is actually owned by weakref->wr_object */

    PyLinkedList weakref_links;
};

PyAPI_DATA(PyTypeObject) _PyWeakBinding_Type;

#define PyWeakBinding_Check(op) (Py_TYPE(op) == &_PyWeakBinding_Type)


#ifdef __cplusplus
}
#endif
#endif /* !Py_WEAKREFOBJECT_H */
