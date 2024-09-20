#ifndef Py_CEVAL_H
#define Py_CEVAL_H
#ifdef __cplusplus
extern "C" {
#endif


/* Interface to random parts in ceval.c */

PyAPI_FUNC(PyObject *) PyEval_CallObjectWithKeywords(
	PyObject *, PyObject *, PyObject *);

/* DLL-level Backwards compatibility: */
#undef PyEval_CallObject
PyAPI_FUNC(PyObject *) PyEval_CallObject(PyObject *, PyObject *);

/* Inline this */
#define PyEval_CallObject(func,arg) \
        PyEval_CallObjectWithKeywords(func, arg, (PyObject *)NULL)

PyAPI_FUNC(PyObject *) PyEval_CallFunction(PyObject *obj,
                                           const char *format, ...);
PyAPI_FUNC(PyObject *) PyEval_CallMethod(PyObject *obj,
                                         const char *methodname,
                                         const char *format, ...);

PyAPI_FUNC(void) PyEval_SetProfile(Py_tracefunc, PyObject *);
PyAPI_FUNC(void) PyEval_SetTrace(Py_tracefunc, PyObject *);

struct _frame; /* Avoid including frameobject.h */

PyAPI_FUNC(PyObject *) PyEval_GetBuiltins(void);
PyAPI_FUNC(PyObject *) PyEval_GetGlobals(void);
PyAPI_FUNC(PyObject *) PyEval_GetLocals(void);
PyAPI_FUNC(struct _frame *) PyEval_GetFrame(void);

/* Look at the current frame's (if any) code's co_flags, and turn on
   the corresponding compiler flags in cf->cf_flags.  Return 1 if any
   flag was set, else return 0. */
PyAPI_FUNC(int) PyEval_MergeCompilerFlags(PyCompilerFlags *cf);

/* Protection against deeply nested recursive calls */
PyAPI_FUNC(void) Py_SetRecursionLimit(int);
PyAPI_FUNC(int) Py_GetRecursionLimit(void);

#define Py_EnterRecursiveCall(where)                                    \
	    (_Py_MakeRecCheck(PyThreadState_Get()->recursion_depth) &&  \
	     _Py_CheckRecursiveCall(where))
#define Py_LeaveRecursiveCall()				\
    do{ if((--PyThreadState_Get()->recursion_depth) <   \
	   _Py_CheckRecursionLimit - 50);               \
	  PyThreadState_Get()->overflowed = 0;          \
    } while(0)
PyAPI_FUNC(int) _Py_CheckRecursiveCall(char *where);
PyAPI_DATA(int) _Py_CheckRecursionLimit;
#ifdef USE_STACKCHECK
#  define _Py_MakeRecCheck(x)  (++(x) > --_Py_CheckRecursionLimit)
#else
#  define _Py_MakeRecCheck(x)  (++(x) > _Py_CheckRecursionLimit)
#endif

#define Py_ALLOW_RECURSION \
  do { unsigned char _old = PyThreadState_Get()->recursion_critical;\
    PyThreadState_Get()->recursion_critical = 1;

#define Py_END_ALLOW_RECURSION \
    PyThreadState_Get()->recursion_critical = _old; \
  } while(0);

PyAPI_FUNC(const char *) PyEval_GetFuncName(PyObject *);
PyAPI_FUNC(const char *) PyEval_GetFuncDesc(PyObject *);

PyAPI_FUNC(PyObject *) PyEval_GetCallStats(PyObject *);
PyAPI_FUNC(PyObject *) PyEval_EvalFrame(struct _frame *);
PyAPI_FUNC(PyObject *) PyEval_EvalFrameEx(struct _frame *f, int exc);

/* this used to be handled on a per-thread basis - now just two globals */
PyAPI_DATA(volatile int) _Py_Ticker;
PyAPI_DATA(int) _Py_CheckInterval;


#ifdef __cplusplus
}
#endif
#endif /* !Py_CEVAL_H */
