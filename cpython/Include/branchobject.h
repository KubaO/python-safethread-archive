/* Branch object */

#ifndef Py_BRANCHOBJECT_H
#define Py_BRANCHOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#include "pythread.h"
#include "pystate.h"


struct _PyCancelObject; /* Avoid including cancelobject.h */

struct _PyBranchObject;

typedef struct _PyBranchChild {
    PyState *pystate;
    struct _PyCancelObject *cancel_scope;
    struct _PyBranchObject *branch;
    PyObject *func;
    PyObject *args;
    PyObject *kwds;
    int save_result;
    PyObject *result;
    PyObject *exception;
    PyThread_type_flag *dead;
    PyLinkedListNode children_links;
    PyLinkedListNode alive_links; /* Also used by deletable */
    PyWaitFor waitfor;
} PyBranchChild;

typedef struct _PyBranchObject {
    PyObject_HEAD
    PyCritical *crit;
    int col_state;

    PyObject *col_ownerthread;
    PyObject *col_threads;
    PyBranchChild *col_mainthread;
    PyLinkedList children;
    PyLinkedList alive;
    PyLinkedList deletable;

    struct _PyCancelObject *col_basecancel;

    int col_cancelling;
    Py_ssize_t col_resultcount;
    Py_ssize_t col_exceptioncount;
} PyBranchObject;

PyAPI_DATA(PyTypeObject) PyBranch_Type;

#define PyBranch_Check(op) PyObject_TypeCheck(op, &PyBranch_Type)
#define PyBranch_CheckExact(op) (Py_TYPE(op) == &PyBranch_Type)

#define BRANCH_NEW      1
#define BRANCH_ALIVE    2
#define BRANCH_DYING    3
#define BRANCH_DEAD     4


#ifdef __cplusplus
}
#endif
#endif /* !Py_BRANCHOBJECT_H */
