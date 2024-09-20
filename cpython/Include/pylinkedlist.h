/* Weak references objects for Python. */

#ifndef Py_LINKEDLIST_H
#define Py_LINKEDLIST_H
#ifdef __cplusplus
extern "C" {
#endif


/* Needed for offsetof */
#include "structmember.h"


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


#ifdef __cplusplus
}
#endif
#endif /* !Py_LINKEDLIST_H */
