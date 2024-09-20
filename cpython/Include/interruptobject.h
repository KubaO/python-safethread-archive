/* Interrupt Object */

#ifndef Py_INTERRUPTOBJECT_H
#define Py_INTERRUPTOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#include "pythread.h"


struct _PyInterruptQueue;

typedef struct _PyInterruptObject {
	PyObject_HEAD
	PyThread_type_lock lock;
	int interrupted;
	struct _PyInterruptObject *parent;
	struct _PyInterruptObject *child;

	/* Either the C or the Python version will be set, but not both */
	void (*notify_parent_int_c)(struct _PyInterruptQueue *, void *arg);
	void *arg;
	PyObject *notify_parent_int_python;

	/* Used only when notify_parent_int_python is used */
	struct _PyInterruptObject *next;
} PyInterruptObject;

typedef struct _PyInterruptQueue {
	PyInterruptObject *head;
	PyInterruptObject *tail;
} PyInterruptQueue;


PyAPI_DATA(PyTypeObject) PyInterrupt_Type;
#define PyInterrupt_Check(op) (Py_Type(op) == &PyInterrupt_Type)

PyAPI_FUNC(PyInterruptObject *) PyInterrupt_New(
	void (*)(struct _PyInterruptQueue *, void *),
	void *, PyObject *);
PyAPI_FUNC(void) PyInterrupt_Push(PyInterruptObject *);
PyAPI_FUNC(void) PyInterrupt_Pop(PyInterruptObject *);

/* Init and Add will only run C functions, so they can be called while
 * you hold a lock.  Finish calls any remaining python functions, so it
 * should be called after you release your lock. */
PyAPI_FUNC(void) PyInterruptQueue_Init(PyInterruptQueue *);
PyAPI_FUNC(void) PyInterruptQueue_Add(PyInterruptQueue *, PyInterruptObject *);
PyAPI_FUNC(void) PyInterruptQueue_AddFromParent(PyInterruptQueue *, PyInterruptObject *);
PyAPI_FUNC(void) PyInterruptQueue_Finish(PyInterruptQueue *);


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERRUPTOBJECT_H */
