/* Weak references objects for Python. */

#ifndef Py_LINKEDLIST_H
#define Py_LINKEDLIST_H
#ifdef __cplusplus
extern "C" {
#endif


/* Needed for offsetof */
#include "structmember.h"


/* Internally, each PyLinkedListNode stores pointers directly other
 * PyLinkedListNodes, not to the overall object, which makes them inner
 * pointers.  The primary advantage is it allows using a sentinel, which
 * in turn allows removing a node from a linked list without knowing
 * exactly which linked list contains it. */
typedef struct _PyLinkedListNode PyLinkedListNode;
struct _PyLinkedListNode {
    PyLinkedListNode *_prev;
    PyLinkedListNode *_next;
};

typedef struct _PyLinkedList {
    PyLinkedListNode _sentinel;
    size_t _offset;
} PyLinkedList;


static inline int PyLinkedList_Empty(PyLinkedList *base);
static inline int PyLinkedList_Detached(PyLinkedListNode *node);


#define _PyLinkedList_ToNode(op, offset) \
    ((PyLinkedListNode *)(((char *)op) + (offset)))
#define _PyLinkedList_FromNode(node, offset) \
    ((void *)(((char *)node) - (offset)))


static inline void
PyLinkedList_InitBase(PyLinkedList *base, size_t offset)
{
    base->_sentinel._prev = &base->_sentinel;
    base->_sentinel._next = &base->_sentinel;
    base->_offset = offset;
}

static inline void
PyLinkedList_InitNode(PyLinkedListNode *node)
{
    node->_prev = NULL;
    node->_next = NULL;
}

static inline void *
PyLinkedList_First(PyLinkedList *base)
{
    if (PyLinkedList_Empty(base))
        return NULL;
    else
        return _PyLinkedList_FromNode(base->_sentinel._next, base->_offset);
}

static inline void *
PyLinkedList_Last(PyLinkedList *base)
{
    if (PyLinkedList_Empty(base))
        return NULL;
    else
        return _PyLinkedList_FromNode(base->_sentinel._prev, base->_offset);
}

static inline void *
PyLinkedList_Before(PyLinkedList *base, void *op)
{
    PyLinkedListNode *node;

    assert(op != NULL);

    node = _PyLinkedList_ToNode(op, base->_offset);
    assert(!PyLinkedList_Detached(node));
    if (node->_prev == &base->_sentinel)
        return NULL;
    return _PyLinkedList_FromNode(node->_prev, base->_offset);
}

static inline void *
PyLinkedList_After(PyLinkedList *base, void *op)
{
    PyLinkedListNode *node;

    assert(op != NULL);

    node = _PyLinkedList_ToNode(op, base->_offset);
    assert(!PyLinkedList_Detached(node));
    if (node->_next == &base->_sentinel)
        return NULL;
    return _PyLinkedList_FromNode(node->_next, base->_offset);
}

static inline void
_PyLinkedList_InsertBefore(PyLinkedListNode *a, PyLinkedListNode *b)
{
    assert(a->_prev != NULL && a->_next != NULL);
    assert(b->_prev == NULL && b->_next == NULL);

    b->_prev = a->_prev;
    a->_prev->_next = b;

    b->_next = a;
    a->_prev = b;
}

static inline void
PyLinkedList_Append(PyLinkedList *base, void *op)
{
    PyLinkedListNode *node = _PyLinkedList_ToNode(op, base->_offset);
    _PyLinkedList_InsertBefore(&base->_sentinel, node);
}

static inline void
PyLinkedList_Remove(PyLinkedListNode *node)
{
    assert(node->_prev != NULL && node->_next != NULL);
    assert(node->_prev != node && node->_next != node);

    node->_prev->_next = node->_next;
    node->_next->_prev = node->_prev;

    node->_prev = NULL;
    node->_next = NULL;
}

/*
 * Iterate over a LinkedList.  Use like so:
 *
 *     PyBranchObject *child = NULL;
 *
 *     while (PyLinkedList_Next(&self->children, &child))
 *         Py_VISIT(child);
 *
 * CAUTION:  It is not safe to remove the current node while iterating.
 *           Removing other nodes is okay.
 */
static inline void *
_PyLinkedList_Next(PyLinkedList *base, void *op)
{
    PyLinkedListNode *node;

    if (op == NULL)
        node = &base->_sentinel;
    else
        node = _PyLinkedList_ToNode(op, base->_offset);

    assert(node->_next != NULL);

    if (node->_next == &base->_sentinel)
        return NULL;
    else
        return _PyLinkedList_FromNode(node->_next, base->_offset);
}
/* Passing in a void ** doesn't do the right thing, so this fancy macro
 * lets us get the right result with the desired API */
#define PyLinkedList_Next(base, op) \
    ((*(op) = (_PyLinkedList_Next((base), *(op)))) != NULL)

static inline int
PyLinkedList_Empty(PyLinkedList *base)
{
    PyLinkedListNode *sentinel = &base->_sentinel;
    assert(sentinel->_prev != NULL && sentinel->_next != NULL);

    if (sentinel->_next == sentinel) {
        assert(sentinel->_prev == sentinel);
        return 1;
    } else {
        assert(sentinel->_prev != sentinel);
        return 0;
    }
}

static inline int
PyLinkedList_Detached(PyLinkedListNode *node)
{
    assert(node->_prev != node && node->_next != node);

    if (node->_next == NULL) {
        assert(node->_prev == NULL);
        return 1;
    } else {
        assert(node->_prev != NULL);
        return 0;
    }
}


#ifdef __cplusplus
}
#endif
#endif /* !Py_LINKEDLIST_H */
