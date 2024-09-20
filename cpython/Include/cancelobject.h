/* Cancel Object */

#ifndef Py_CANCELOBJECT_H
#define Py_CANCELOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#include "pythread.h"


struct _PyCancelQueue;

typedef struct _PyCancelObject {
    PyObject_HEAD
    int cancelled;
    PyState *pystate;
    PyLinkedListNode stack_links;

    void (*callback)(struct _PyCancelQueue *, void *arg);
    void *arg;
    int callback_activated;
    PyThread_type_flag *callback_finished;

    PyLinkedListNode queue_links;
} PyCancelObject;

typedef struct _PyCancelQueue {
    PyLinkedList list;
} PyCancelQueue;


PyAPI_DATA(PyTypeObject) PyCancel_Type;
#define PyCancel_Check(op) (Py_TYPE(op) == &PyCancel_Type)

PyAPI_FUNC(PyCancelObject *) PyCancel_New(
    void (*)(PyCancelQueue *, void *),
    void *, PyState *);
PyAPI_FUNC(void) PyCancel_Push(PyCancelObject *);
PyAPI_FUNC(void) PyCancel_Pop(PyCancelObject *);

/* Cancel marks a given PyCancelObject as cancelled, notifying children 
 * of this.  This childrens' callbacks are not called until Finish. */
PyAPI_FUNC(void) PyCancelQueue_Init(PyCancelQueue *);
PyAPI_FUNC(void) PyCancelQueue_Cancel(PyCancelQueue *, PyCancelObject *);
PyAPI_FUNC(void) PyCancelQueue_Finish(PyCancelQueue *);


#ifdef __cplusplus
}
#endif
#endif /* !Py_CANCELOBJECT_H */
