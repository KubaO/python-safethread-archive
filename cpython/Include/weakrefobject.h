/* Weak references objects for Python. */

#ifndef Py_WEAKREFOBJECT_H
#define Py_WEAKREFOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif


/* XXX This should get moved into its own file */
/* The way PyLinkedList is used is that all objects within a given
 * linked list have the same layout, allowing you to use
 * PyLinkedList_Restore to get back the original object.
 *
 * The sentinel needs special handling, either by checking for it before
 * calling PyLinkedList_Restore, or by making it a valid object and
 * checking after. */
typedef struct _PyLinkedList PyLinkedList;
struct _PyLinkedList {
    PyLinkedList *prev;
    PyLinkedList *next;
};

#define PyLinkedList_Restore(type, field, op) \
    (type *)(((char *)(op)) - offsetof(type, field))

#define PyLinkedList_Append(sentinel, op) PyLinkedList_InsertBefore(sentinel, op)
static inline void
PyLinkedList_InsertBefore(PyLinkedList *a, PyLinkedList *b)
{
    assert(a->prev != NULL && a->next != NULL);
    assert(b->prev == NULL && b->next == NULL);

    b->prev = a->prev;
    a->prev->next = b;

    b->next = a;
    a->prev = b;
}

static inline void
PyLinkedList_Remove(PyLinkedList *op)
{
    assert(op->prev != NULL && op->next != NULL);
    assert(op->prev != op && op->next != op);

    op->prev->next = op->next;
    op->next->prev = op->prev;

    op->prev = NULL;
    op->next = NULL;
}

/*
 * Iterate over a LinkedList.  Use like so:
 *
 *     PyLinkedList *handle_links = &queue->live_links;
 *
 *     while (PyLinkedList_Next(&queue->live_links, &handle_links)) {
 *         handle = PyLinkedList_Restore(PyDeathQueueHandle, queue_links,
 *             handle_links);
 *         Py_VISIT(handle);
 *     }
 *
 * CAUTION:  It isn't safe to modify the LinkedList while iterating.
 */
static inline int
PyLinkedList_Next(PyLinkedList *sentinel, PyLinkedList **op)
{
    if ((*op)->next == sentinel)
        return 0;
    else {
        *op = (*op)->next;
        return 1;
    }
}

static inline int
PyLinkedList_Empty(PyLinkedList *sentinel)
{
    assert(sentinel->prev != NULL && sentinel->next != NULL);

    if (sentinel->next == sentinel) {
        assert(sentinel->prev == sentinel);
        return 1;
    } else {
        assert(sentinel->prev != sentinel);
        return 0;
    }
}

static inline int
PyLinkedList_Detatched(PyLinkedList *op)
{
    assert(op->prev != op && op->next != op);

    if (op->next == NULL) {
        assert(op->prev == NULL);
        return 1;
    } else {
        assert(op->prev != NULL);
        return 0;
    }
}


typedef struct _PyWeakReference PyWeakReference;
typedef struct _PyDeathQueueHandle PyDeathQueueHandle;
typedef struct _PyDeathQueue PyDeathQueue;

struct _PyWeakReference {
    PyObject_HEAD

    /* Critical section protecting access to wr_object and our (not yet
     * implemented) death notice list.  hash is NOT protected */
    PyCritical *crit;

    /* The object to which this is a weak reference, or Py_None if none.
     * Note that this is a stealth reference:  wr_object's refcount is
     * not incremented to reflect this pointer.
     */
    PyObject *wr_object;

    /* A cache for wr_object's hash code.  As usual for hashes, this is -1
     * if the hash code isn't known yet.
     */
    AO_t hash;

    /* If wr_object is weakly referenced, wr_object has a doubly-linked NULL-
     * terminated list of weak references to it.  These are the list pointers.
     * If wr_object goes away, wr_object is set to Py_None, and these pointers
     * have no meaning then.
     */
    //PyWeakReference *wr_prev;
    //PyWeakReference *wr_next;
    PyLinkedList handles;
};

PyAPI_DATA(PyTypeObject) _PyWeakref_Type;

#define PyWeakref_Check(op) (Py_Type(op) == &_PyWeakref_Type)
#define PyWeakref_CheckRefExact PyWeakref_Check
#define PyWeakref_CheckRef PyWeakref_Check


PyAPI_FUNC(PyObject *) PyWeakref_NewRef(PyObject *ob,
                                              PyObject *callback);
/* Note that this DOES incref the returned object, if not NULL! */
PyAPI_FUNC(PyObject *) PyWeakref_GetObjectEx(PyObject *ref);


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
    (Py_Type(op) == &_PyDeathQueueHandle_Type)


struct _PyDeathQueue {
    PyObject_HEAD

    PyCritical *crit;

    PyLinkedList live_links;
    PyLinkedList dead_links;
};

PyAPI_DATA(PyTypeObject) _PyDeathQueue_Type;

#define PyDeathQueue_Check(op) (Py_Type(op) == &_PyDeathQueue_Type)


#ifdef __cplusplus
}
#endif
#endif /* !Py_WEAKREFOBJECT_H */
