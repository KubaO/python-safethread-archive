#ifndef Py_PYSIGNAL_H
#define Py_PYSIGNAL_H
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PyOS_sighandler_t)(int);
PyAPI_FUNC(PyOS_sighandler_t) PyOS_getsig(int);
PyAPI_FUNC(PyOS_sighandler_t) PyOS_setsig(int, PyOS_sighandler_t);

PyAPI_FUNC(void) PyOS_AfterFork(void);

#ifdef __cplusplus
}
#endif
#endif /* !Py_PYSIGNAL_H */
