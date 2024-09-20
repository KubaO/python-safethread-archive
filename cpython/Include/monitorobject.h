/* Monitor object and Monitor Space interface */

#ifndef Py_MONITOROBJECT_H
#define Py_MONITOROBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#include "pythread.h"


typedef struct {
	PyObject_HEAD
	PyObject *mon_monitorspace;  /* The monitorspace that contains us */
} PyMonitorObject;

typedef struct _PyMonitorSpaceObject {
	PyObject_HEAD
	PyThread_type_lock lock;
	/* XXX FIXME rename struct _ts */
	struct _ts *lock_holder;
	struct _ts *first_waiter;
	struct _ts *last_waiter;
	/* XXX flag (or counter?) used by PyState_StopTheWorld */
} PyMonitorSpaceObject;

PyAPI_DATA(PyTypeObject) PyMonitorMeta_Type;
PyAPI_DATA(PyTypeObject) PyMonitor_Type;
PyAPI_DATA(PyTypeObject) PyMonitorSpace_Type;

#define PyMonitorMeta_Check(op) PyObject_TypeCheck(op, &PyMonitorMeta_Type)
#define PyMonitorMeta_CheckExact(op) ((op)->ob_type == &PyMonitorMeta_Type)

#define PyMonitor_Check(op) \
	PyType_FastSubclass((op)->ob_type, Py_TPFLAGS_MONITOR_SUBCLASS)
#define PyMonitor_CheckExact(op) ((op)->ob_type == &PyMonitor_Type)

#define PyMonitorSpace_Check(op) PyObject_TypeCheck(op, &PyMonitorSpace_Type)
#define PyMonitorSpace_CheckExact(op) ((op)->ob_type == &PyMonitorSpace_Type)

#define PyMonitor_GetMonitorSpace(op) \
	((PyMonitorSpaceObject *)(((PyMonitorObject *)op)->mon_monitorspace))

PyAPI_FUNC(int) PyMonitorSpace_IsCurrent(struct _PyMonitorSpaceObject *);
PyAPI_FUNC(PyObject *) PyMonitorSpace_GetCurrent(void);


#ifdef __cplusplus
}
#endif
#endif /* !Py_MONITOROBJECT_H */
