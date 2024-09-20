/* Collate object */

#ifndef Py_COLLATEOBJECT_H
#define Py_COLLATEOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#include "pythread.h"
#include "pystate.h"


struct _PyInterruptObject; /* Avoid including interruptobject.h */

struct _PyCollateObject;

typedef struct {
	PyObject *e_type;
	PyObject *e_value;
	PyObject *e_traceback;
} PyExcBox;

typedef struct _PyCollateChild {
	PyInterpreterState *interp;
	PyThreadState *tstate;
	struct _PyInterruptObject *interrupt_point;
	struct _PyCollateObject *collate;
	PyObject *func;
	PyObject *args;
	PyObject *kwds;
	int save_result;
	PyObject *result;
	PyExcBox failure;
	struct _PyCollateChild *prev;
	struct _PyCollateChild *next;
} PyCollateChild;

typedef struct _PyCollateObject {
	PyObject_HEAD
	PyThread_type_lock col_lock;
	int col_state;

	PyObject *col_ownerthread;
	PyObject *col_threads;
	PyCollateChild *col_mainthread;
	PyCollateChild *col_head;
	PyCollateChild *col_tail;
	Py_ssize_t col_threadcount;
	PyThread_type_sem col_nothreads;

	struct _PyInterruptObject *col_baseinterrupt;

	int col_interrupting;
	Py_ssize_t col_resultcount;
	Py_ssize_t col_failurecount;
} PyCollateObject;

PyAPI_DATA(PyTypeObject) PyCollate_Type;

#define PyCollate_Check(op) PyObject_TypeCheck(op, &PyCollate_Type)
#define PyCollate_CheckExact(op) (Py_Type(op) == &PyCollate_Type)

#define COLLATE_NEW	1
#define COLLATE_ALIVE	2
#define COLLATE_DYING	3
#define COLLATE_DEAD	4


#ifdef __cplusplus
}
#endif
#endif /* !Py_COLLATEOBJECT_H */
