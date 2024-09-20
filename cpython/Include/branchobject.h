/* Branch object */

#ifndef Py_BRANCHOBJECT_H
#define Py_BRANCHOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#include "pythread.h"
#include "pystate.h"


struct _PyInterruptObject; /* Avoid including interruptobject.h */

struct _PyBranchObject;

typedef struct _PyBranchChild {
	PyInterpreterState *interp;
	PyThreadState *tstate;
	struct _PyInterruptObject *interrupt_point;
	struct _PyBranchObject *branch;
	PyObject *func;
	PyObject *args;
	PyObject *kwds;
	int save_result;
	PyObject *result;
	PyObject *exception;
	struct _PyBranchChild *prev;
	struct _PyBranchChild *next;
} PyBranchChild;

typedef struct _PyBranchObject {
	PyObject_HEAD
	PyThread_type_lock col_lock;
	int col_state;

	PyObject *col_ownerthread;
	PyObject *col_threads;
	PyBranchChild *col_mainthread;
	PyBranchChild *col_head;
	PyBranchChild *col_tail;
	Py_ssize_t col_threadcount;
	PyThread_type_sem col_nothreads;

	struct _PyInterruptObject *col_baseinterrupt;

	int col_interrupting;
	Py_ssize_t col_resultcount;
	Py_ssize_t col_exceptioncount;
} PyBranchObject;

PyAPI_DATA(PyTypeObject) PyBranch_Type;

#define PyBranch_Check(op) PyObject_TypeCheck(op, &PyBranch_Type)
#define PyBranch_CheckExact(op) (Py_Type(op) == &PyBranch_Type)

#define BRANCH_NEW	1
#define BRANCH_ALIVE	2
#define BRANCH_DYING	3
#define BRANCH_DEAD	4


#ifdef __cplusplus
}
#endif
#endif /* !Py_BRANCHOBJECT_H */
