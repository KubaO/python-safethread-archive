
/* Interfaces to parse and execute pieces of python code */

#ifndef Py_PYTHONRUN_H
#define Py_PYTHONRUN_H
#ifdef __cplusplus
extern "C" {
#endif

#define PyCF_MASK CO_FUTURE_SHARED_MODULE
#define PyCF_MASK_OBSOLETE 0
#define PyCF_SOURCE_IS_UTF8  0x0100
#define PyCF_DONT_IMPLY_DEDENT 0x0200
#define PyCF_ONLY_AST 0x0400

typedef struct {
	int cf_flags;  /* bitmask of CO_xxx flags relevant to future */
} PyCompilerFlags;

PyAPI_FUNC(void) Py_SetProgramName(wchar_t *);
PyAPI_FUNC(wchar_t *) Py_GetProgramName(void);

PyAPI_FUNC(void) Py_SetPythonHome(wchar_t *);
PyAPI_FUNC(wchar_t *) Py_GetPythonHome(void);

PyAPI_FUNC(void) Py_Initialize(void);
PyAPI_FUNC(void) Py_InitializeEx(int);
PyAPI_FUNC(void) Py_Finalize(void);
PyAPI_FUNC(int) Py_IsInitialized(void);

PyAPI_FUNC(int) PyRun_AnyFileFlags(FILE *, const char *, PyCompilerFlags *);
PyAPI_FUNC(int) PyRun_AnyFileExFlags(FILE *, const char *, int, PyCompilerFlags *);
PyAPI_FUNC(int) PyRun_SimpleStringFlags(const char *, PyCompilerFlags *);
PyAPI_FUNC(int) PyRun_SimpleFileExFlags(FILE *, const char *, int, PyCompilerFlags *);
PyAPI_FUNC(int) PyRun_InteractiveOneFlags(FILE *, const char *, PyCompilerFlags *);
PyAPI_FUNC(int) PyRun_InteractiveLoopFlags(FILE *, const char *, PyCompilerFlags *);

PyAPI_FUNC(struct _mod *) PyParser_ASTFromString(const char *, const char *, 
						 int, PyCompilerFlags *flags,
                                                 PyArena *);
PyAPI_FUNC(struct _mod *) PyParser_ASTFromFile(FILE *, const char *, 
					       const char*, int, 
					       char *, char *,
                                               PyCompilerFlags *, int *,
                                               PyArena *);
#define PyParser_SimpleParseString(S, B) \
        PyParser_SimpleParseStringFlags(S, B, 0)
#define PyParser_SimpleParseFile(FP, S, B) \
        PyParser_SimpleParseFileFlags(FP, S, B, 0)
PyAPI_FUNC(struct _node *) PyParser_SimpleParseStringFlags(const char *, int, 
							  int);
PyAPI_FUNC(struct _node *) PyParser_SimpleParseFileFlags(FILE *, const char *,
							int, int);

PyAPI_FUNC(PyObject *) PyRun_StringFlags(const char *, int, PyObject *, 
					 PyObject *, PyCompilerFlags *);

PyAPI_FUNC(PyObject *) PyRun_FileExFlags(FILE *, const char *, int, 
					 PyObject *, PyObject *, int, 
					 PyCompilerFlags *);

#define Py_CompileString(str, p, s) Py_CompileStringFlags(str, p, s, NULL)
PyAPI_FUNC(PyObject *) Py_CompileStringFlags(const char *, const char *, int,
					     PyCompilerFlags *);
PyAPI_FUNC(struct symtable *) Py_SymtableString(const char *, const char *, int);

PyAPI_FUNC(void) PyErr_Print(void);
PyAPI_FUNC(void) PyErr_PrintEx(int);
PyAPI_FUNC(void) PyErr_Display(PyObject *, PyObject *, PyObject *);

/* Py_PyAtExit is for the atexit module, Py_AtExit is for low-level
 * exit functions.
 */
PyAPI_FUNC(void) _Py_PyAtExit(void (*func)(void));
PyAPI_FUNC(int) Py_AtExit(void (*func)(void));

PyAPI_FUNC(void) Py_Exit(int)
    Py_GCC_ATTRIBUTE((noreturn));

PyAPI_FUNC(int) Py_FdIsInteractive(FILE *, const char *);

/* Bootstrap */
PyAPI_FUNC(int) Py_Main(int argc, wchar_t **argv);

/* Use macros for a bunch of old variants */
#define PyRun_String(str, s, g, l) PyRun_StringFlags(str, s, g, l, NULL)
#define PyRun_AnyFile(fp, name) PyRun_AnyFileExFlags(fp, name, 0, NULL)
#define PyRun_AnyFileEx(fp, name, closeit) \
	PyRun_AnyFileExFlags(fp, name, closeit, NULL)
#define PyRun_AnyFileFlags(fp, name, flags) \
	PyRun_AnyFileExFlags(fp, name, 0, flags)
#define PyRun_SimpleString(s) PyRun_SimpleStringFlags(s, NULL)
#define PyRun_SimpleFile(f, p) PyRun_SimpleFileExFlags(f, p, 0, NULL)
#define PyRun_SimpleFileEx(f, p, c) PyRun_SimpleFileExFlags(f, p, c, NULL)
#define PyRun_InteractiveOne(f, p) PyRun_InteractiveOneFlags(f, p, NULL)
#define PyRun_InteractiveLoop(f, p) PyRun_InteractiveLoopFlags(f, p, NULL)
#define PyRun_File(fp, p, s, g, l) \
        PyRun_FileExFlags(fp, p, s, g, l, 0, NULL)
#define PyRun_FileEx(fp, p, s, g, l, c) \
        PyRun_FileExFlags(fp, p, s, g, l, c, NULL)
#define PyRun_FileFlags(fp, p, s, g, l, flags) \
        PyRun_FileExFlags(fp, p, s, g, l, 0, flags)

/* In getpath.c */
PyAPI_FUNC(wchar_t *) Py_GetProgramFullPath(void);
PyAPI_FUNC(wchar_t *) Py_GetPrefix(void);
PyAPI_FUNC(wchar_t *) Py_GetExecPrefix(void);
PyAPI_FUNC(wchar_t *) Py_GetPath(void);

/* In their own files */
PyAPI_FUNC(const char *) Py_GetVersion(void);
PyAPI_FUNC(const char *) Py_GetPlatform(void);
PyAPI_FUNC(const char *) Py_GetCopyright(void);
PyAPI_FUNC(const char *) Py_GetCompiler(void);
PyAPI_FUNC(const char *) Py_GetBuildInfo(void);
PyAPI_FUNC(const char *) _Py_svnversion(void);
PyAPI_FUNC(const char *) Py_SubversionRevision(void);
PyAPI_FUNC(const char *) Py_SubversionShortBranch(void);

/* Internal -- various one-time initializations */
PyAPI_FUNC(int) _PyBuiltin_Init(void);
PyAPI_FUNC(void) _Py_ThreadTools_Init(void);
PyAPI_FUNC(int) _PySys_Init(void);
PyAPI_FUNC(void) _PyImport_Init(void);
PyAPI_FUNC(void) _PyExc_Init(void);
PyAPI_FUNC(void) _PyImportHooks_Init(void);
PyAPI_FUNC(int) _PyFrame_Init(void);
PyAPI_FUNC(void) _PyFloat_Init(void);
PyAPI_FUNC(int) PyBytes_Init(void);
PyAPI_FUNC(void) _PyMethod_Init(void);
PyAPI_FUNC(void) _PyList_Init(void);
PyAPI_FUNC(void) _PySet_Init(void);
PyAPI_FUNC(void) _PyCFunction_Init(void);
PyAPI_FUNC(void) _PySignal_Init(void);
PyAPI_FUNC(void) _PySignal_InitSigInt(int);

/* Various internal finalizers */
PyAPI_FUNC(void) _PyExc_Fini(void);
PyAPI_FUNC(void) _PyImport_Fini(void);
PyAPI_FUNC(void) PyMethod_Fini(void);
PyAPI_FUNC(void) PyFrame_Fini(void);
PyAPI_FUNC(void) PyCFunction_Fini(void);
PyAPI_FUNC(void) PyDict_Fini(void);
PyAPI_FUNC(void) PyTuple_Fini(void);
PyAPI_FUNC(void) PyList_Fini(void);
PyAPI_FUNC(void) PySet_Fini(void);
PyAPI_FUNC(void) PyString_Fini(void);
PyAPI_FUNC(void) PyBytes_Fini(void);
PyAPI_FUNC(void) PyFloat_Fini(void);
PyAPI_FUNC(void) PyDict_Fini(void);
PyAPI_FUNC(void) _PySignal_Fini(void);
PyAPI_FUNC(void) _PySignal_FiniSigInt(void);

/* Stuff with no proper home (yet) */
PyAPI_FUNC(char *) PyOS_Readline(FILE *, FILE *, char *);
PyAPI_DATA(int) (*PyOS_InputHook)(void);
PyAPI_DATA(char) *(*PyOS_ReadlineFunctionPointer)(FILE *, FILE *, char *);
PyAPI_DATA(PyState *) _PyOS_ReadlineTState;

/* Stack size, in "pointers" (so we get extra safety margins
   on 64-bit platforms).  On a 32-bit platform, this translates
   to a 8k margin. */
#define PYOS_STACK_MARGIN 2048

#if defined(WIN32) && !defined(MS_WIN64) && defined(_MSC_VER)
/* Enable stack checking under Microsoft C */
#define USE_STACKCHECK
#endif

#ifdef USE_STACKCHECK
/* Check that we aren't overflowing our stack */
PyAPI_FUNC(int) PyOS_CheckStack(void);
#endif


#ifdef __cplusplus
}
#endif
#endif /* !Py_PYTHONRUN_H */
