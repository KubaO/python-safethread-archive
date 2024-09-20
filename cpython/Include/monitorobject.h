/* Monitor object and Monitor Space interface */

#ifndef Py_MONITOROBJECT_H
#define Py_MONITOROBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#include "pythread.h"
#include "pystate.h"


struct _PyBranchChild; /* Avoid including branchobject.h */

typedef struct {
    PyObject_HEAD
    PyObject *mon_monitorspace;  /* The monitorspace that contains us */
    PyObject *mon_conditions;  /* All conditions that have been used on us */
    int mon_waking;  /* One condition is true and is being woken */
} PyMonitorObject;

typedef struct _PyMonitorSpaceObject {
    PyObject_HEAD
    PyWaitFor waitfor;
    //PyState *lock_holder;
    //PyState *first_waiter;
    //PyState *last_waiter;
    /* XXX flag (or counter?) used by PyState_StopTheWorld */
    PyLinkedList waiters;
    PyThread_type_cond *idle;
} PyMonitorSpaceObject;

PyAPI_DATA(PyTypeObject) PyMonitorMeta_Type;
PyAPI_DATA(PyTypeObject) PyMonitor_Type;
PyAPI_DATA(PyTypeObject) PyMonitorMethod_Type;
PyAPI_DATA(PyTypeObject) PyBoundMonitorMethod_Type;
PyAPI_DATA(PyTypeObject) PyMonitorCondition_Type;
PyAPI_DATA(PyTypeObject) PyBoundMonitorCondition_Type;
PyAPI_DATA(PyTypeObject) PyMonitorSpace_Type;

#define PyMonitorMeta_Check(op) PyObject_TypeCheck(op, &PyMonitorMeta_Type)
#define PyMonitorMeta_CheckExact(op) (Py_TYPE(op) == &PyMonitorMeta_Type)

#define PyMonitor_Check(op) \
    PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_MONITOR_SUBCLASS)
#define PyMonitor_CheckExact(op) (Py_TYPE(op) == &PyMonitor_Type)

#define PyCondition_Check(op) PyObject_TypeCheck(op, &PyMonitorCondition_Type)
#define PyCondition_CheckExact(op) (Py_TYPE(op) == &PyMonitorCondition_Type)

#define PyMonitorSpace_Check(op) PyObject_TypeCheck(op, &PyMonitorSpace_Type)
#define PyMonitorSpace_CheckExact(op) (Py_TYPE(op) == &PyMonitorSpace_Type)

#define PyMonitor_GetMonitorSpace(op) \
    ((PyMonitorSpaceObject *)(((PyMonitorObject *)op)->mon_monitorspace))

PyAPI_FUNC(int) PyMonitorSpace_IsCurrent(struct _PyMonitorSpaceObject *);
PyAPI_FUNC(PyObject *) PyMonitorSpace_GetCurrent(void);
PyAPI_FUNC(void) PyMonitorSpace_SetDeadlockDelay(double);
PyAPI_FUNC(double) PyMonitorSpace_GetDeadlockDelay(void);
PyAPI_FUNC(void) _PyMonitorSpace_WaitForBranchChild(struct _PyBranchChild *);
PyAPI_FUNC(void) _PyMonitorSpace_BlockOnSelf(PyWaitFor *);
PyAPI_FUNC(void) _PyMonitorSpace_UnblockOnSelf(PyWaitFor *);

PyAPI_FUNC(PyObject *) PyMonitorMethod_New(PyObject *);
PyAPI_FUNC(PyObject *) PyBoundMonitorMethod_New(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) PyMonitorCondition_New(PyObject *);
PyAPI_FUNC(PyObject *) PyBoundMonitorCondition_New(PyObject *, PyObject *);


#ifdef __cplusplus
}
#endif
#endif /* !Py_MONITOROBJECT_H */
