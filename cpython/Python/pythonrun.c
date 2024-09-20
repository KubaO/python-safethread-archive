
/* Python interpreter top-level routines, including init/exit */

#include "Python.h"

#include "Python-ast.h"
#undef Yield /* undefine macro conflicting with winbase.h */
#include "grammar.h"
#include "node.h"
#include "token.h"
#include "parsetok.h"
#include "errcode.h"
#include "code.h"
#include "compile.h"
#include "symtable.h"
#include "pyarena.h"
#include "ast.h"
#include "eval.h"
#include "marshal.h"
#include "branchobject.h"

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif

#ifdef HAVE_LANGINFO_H
#include <locale.h>
#include <langinfo.h>
#endif

#ifdef MS_WINDOWS
#undef BYTE
#include "windows.h"
#endif

#ifndef Py_REF_DEBUG
#define PRINT_TOTAL_REFS()
#else /* Py_REF_DEBUG */
#define PRINT_TOTAL_REFS() fprintf(stderr,				\
				   "[%" PY_FORMAT_SIZE_T "d refs]\n",	\
				   _Py_GetRefTotal())
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern char *Py_GetPath(void);

extern grammar _PyParser_Grammar; /* From graminit.c */

/* Forward */
static void initfinalize(void);
static void finifinalize(void);
static void initmain(void);
static void initsite(void);
static int initstdio(void);
static PyObject *run_mod(mod_ty, const char *, PyObject *, PyObject *,
			  PyCompilerFlags *, PyArena *);
static PyObject *run_pyc_file(FILE *, const char *, PyObject *, PyObject *,
			      PyCompilerFlags *);
static void err_input(perrdetail *);
static void call_py_exitfuncs(void);
static void call_ll_exitfuncs(void);
extern void _PyGC_Init(void);
extern void _PyUnicode_PreInit(void);
extern void _PyUnicode_Init(void);
extern void _PyUnicode_Fini(void);
extern void _PyUnicode_PostFini(void);
extern void _PyDict_PreInit(void);
extern int _PyLong_Init(void);
extern void PyLong_Fini(void);
extern void _PyAbstract_Init(void);

extern void _PyState_InitThreads(void);
extern void _PyState_ClearThreads(void);
extern void _PyState_Fini(void);

extern void _Py_Refchain_Init(void);
extern void _Py_Refchain_Fini(void);

int Py_DebugFlag; /* Needed by parser.c */
int Py_VerboseFlag; /* Needed by import.c */
int Py_InteractiveFlag; /* Needed by Py_FdIsInteractive() below */
int Py_InspectFlag; /* Needed to determine whether to exit at SystemError */
int Py_NoSiteFlag; /* Suppress 'import site' */
int Py_BytesWarningFlag; /* Warn on str(bytes) and str(buffer) */
int Py_DontWriteBytecodeFlag; /* Suppress writing bytecode files (*.py[co]) */
int Py_UseClassExceptionsFlag = 1; /* Needed by bltinmodule.c: deprecated */
int Py_FrozenFlag; /* Needed by getpath.c */
int Py_IgnoreEnvironmentFlag; /* e.g. PYTHONPATH, PYTHONHOME */

static PyState_EnterFrame *enterframe;

/* Reference to 'warnings' module, to avoid importing it
   on the fly when the import lock may be held.  See 683658/771097
*/
static PyObject *warnings_module = NULL;

static PyObject *finalize_branch = NULL;

/* Returns a borrowed reference to the 'warnings' module, or NULL.
   If the module is returned, it is guaranteed to have been obtained
   without acquiring the import lock
*/
PyObject *PyModule_GetWarningsModule(void)
{
	PyObject *typ, *val, *tb;
	PyObject *all_modules;
	/* If we managed to get the module at init time, just use it */
	if (warnings_module)
		return warnings_module;
	/* If it wasn't available at init time, it may be available
	   now in sys.modules (common scenario is frozen apps: import
	   at init time fails, but the frozen init code sets up sys.path
	   correctly, then does an implicit import of warnings for us
	*/
	/* Save and restore any exceptions */
	PyErr_Fetch(&typ, &val, &tb);

	all_modules = PySys_GetObject("modules");
	if (all_modules) {
		warnings_module = PyDict_GetItemString(all_modules, "warnings");
		/* We keep a ref in the global */
		Py_XINCREF(warnings_module);
	}
	PyErr_Restore(typ, val, tb);
	return warnings_module;
}

/* 0 for uninitialized, 1 for initialized, -1 for finalized */
static int initialized = 0;

/* API to access the initialized flag -- useful for esoteric use */

int
Py_IsInitialized(void)
{
    /* Typical callers don't want to know we're finalized, so we make
     * sure they don't have to test for it. */
    return initialized == 1;
}

/* Global initializations.  Can be undone by Py_Finalize().  Don't
   call this twice without an intervening Py_Finalize() call.  When
   initializations fail, a fatal error is issued and the function does
   not return.  On return, the first thread and interpreter state have
   been created.

   Locking: you must hold the interpreter lock while calling this.
   (If the lock has not yet been initialized, that's equivalent to
   having the lock, but you cannot use multiple threads.)

*/

static int
add_flag(int flag, const char *envs)
{
	int env = atoi(envs);
	if (flag < env)
		flag = env;
	if (flag < 1)
		flag = 1;
	return flag;
}

void
Py_InitializeEx(int handle_sigint)
{
	PyObject *pstderr;
	char *p;
#if defined(HAVE_LANGINFO_H) && defined(CODESET)
	char *codeset;
#endif
	extern void _Py_ReadyTypes(void);

	if (initialized == 1)
		Py_FatalError("Cannot call Py_Initialize: already initialized");
	if (initialized == -1)
		Py_FatalError("Cannot call Py_Initialize: already finalized");
	if (initialized != 0)
		Py_FatalError("Cannot call Py_Initialize: unknown initialized stated");
	initialized = 1;

#ifdef HAVE_SETLOCALE
	/* Set up the LC_CTYPE locale, so we can obtain
	   the locale's charset without having to switch
	   locales. */
	setlocale(LC_CTYPE, "");
#endif

	if ((p = Py_GETENV("PYTHONDEBUG")) && *p != '\0')
		Py_DebugFlag = add_flag(Py_DebugFlag, p);
	if ((p = Py_GETENV("PYTHONVERBOSE")) && *p != '\0')
		Py_VerboseFlag = add_flag(Py_VerboseFlag, p);
	if ((p = Py_GETENV("PYTHONOPTIMIZE")) && *p != '\0')
		Py_OptimizeFlag = add_flag(Py_OptimizeFlag, p);
	if ((p = Py_GETENV("PYTHONDONTWRITEBYTECODE")) && *p != '\0')
		Py_DontWriteBytecodeFlag = add_flag(Py_DontWriteBytecodeFlag, p);

	_PyGC_Init();

	_Py_Refchain_Init();

	_PyState_InitThreads();

	enterframe = PyState_Enter();
	if (enterframe == NULL)
		Py_FatalError("Py_Initialize: PySpace_Enter failed");

	/* Bare minimum before other types */
	_PyUnicode_PreInit();
	_PyDict_PreInit();

	if (!_PyFrame_Init())
		Py_FatalError("Py_Initialize: can't init frames");
	_PyMethod_Init();
	if (!_PyLong_Init())
		Py_FatalError("Py_Initialize: can't init longs");
	_PyFloat_Init();
	_PyList_Init();
	_PySet_Init();
	if (!PyBytes_Init())
		Py_FatalError("Py_Initialize: can't init bytes");
	_PyCFunction_Init();

	/* Init Unicode implementation; relies on the codec registry */
	_PyUnicode_Init();

	_PyAbstract_Init();

	_Py_ReadyTypes();

	_PyImport_Init();

	if (_PyBuiltin_Init())
		Py_FatalError("Py_Initialize: can't initialize builtins modules");

	_Py_ThreadTools_Init();

	/* initialize builtin exceptions */
	_PyExc_Init();

	if (_PySys_Init())
		Py_FatalError("Py_Initialize: can't initialize sys");
	_PyImport_FixupExtension("sys", "sys");
	PySys_SetPath(Py_GetPath());
	PySys_SetObject("modules", PyImport_GetModuleDict());

	/* Set up a preliminary stderr printer until we have enough
	   infrastructure for the io module in place. */
	pstderr = PyFile_NewStdPrinter(fileno(stderr));
	if (pstderr == NULL)
		Py_FatalError("Py_Initialize: can't set preliminary stderr");
	PySys_SetObject("stderr", pstderr);
	PySys_SetObject("__stderr__", pstderr);

	/* phase 2 of builtins */
	_PyImport_FixupExtension("builtins", "builtins");

	_PyImportHooks_Init();

	_PySignal_Init();

	initfinalize();

	_PySignal_InitSigInt(handle_sigint);

	initmain(); /* Module __main__ */
	if (initstdio() < 0)
		Py_FatalError(
		    "Py_Initialize: can't initialize sys standard streams");
	if (!Py_NoSiteFlag)
		initsite(); /* Module site */

	warnings_module = PyImport_ImportModule("warnings");
	if (!warnings_module) {
		PyErr_Clear();
	}
	else {
		PyObject *o;
		char *action[8];

		if (Py_BytesWarningFlag > 1)
			*action = "error";
		else if (Py_BytesWarningFlag)
			*action = "default";
		else
			*action = "ignore";

		o = PyObject_CallMethod(warnings_module,
					"simplefilter", "sO",
					*action, PyExc_BytesWarning);
		if (o == NULL)
			Py_FatalError("Py_Initialize: can't initialize"
				      "warning filter for BytesWarning.");
		Py_DECREF(o);
        }

#if defined(HAVE_LANGINFO_H) && defined(CODESET)
	/* On Unix, set the file system encoding according to the
	   user's preference, if the CODESET names a well-known
	   Python codec, and Py_FileSystemDefaultEncoding isn't
	   initialized by other means. Also set the encoding of
	   stdin and stdout if these are terminals.  */

	codeset = nl_langinfo(CODESET);
	if (codeset && *codeset) {
		PyObject *enc = PyCodec_Encoder(codeset);
		if (enc) {
			codeset = strdup(codeset);
			Py_DECREF(enc);
		} else {
			codeset = NULL;
			PyErr_Clear();
		}
	} else
		codeset = NULL;

	if (codeset) {
		if (!Py_FileSystemDefaultEncoding)
			Py_FileSystemDefaultEncoding = codeset;
		else
			free(codeset);
	}
#endif
}

void
Py_Initialize(void)
{
	Py_InitializeEx(1);
}


#ifdef COUNT_ALLOCS
extern void dump_counts(FILE*);
#endif

/* Flush stdout and stderr */

static void
flush_std_files(void)
{
	PyObject *fout = PySys_GetObject("stdout");
	PyObject *ferr = PySys_GetObject("stderr");
	PyObject *tmp;

	if (fout != NULL && fout != Py_None) {
		tmp = PyObject_CallMethod(fout, "flush", "");
		if (tmp == NULL)
			PyErr_Clear();
		else
			Py_DECREF(tmp);
	}

	if (ferr != NULL || ferr != Py_None) {
		tmp = PyObject_CallMethod(ferr, "flush", "");
		if (tmp == NULL)
			PyErr_Clear();
		else
			Py_DECREF(tmp);
	}
}

/* Undo the effect of Py_Initialize().

   Beware: if multiple interpreter and/or thread states exist, these
   are not wiped out; only the current thread and interpreter state
   are deleted.  But since everything else is deleted, those other
   interpreter and thread states should no longer be used.

   (XXX We should do better, e.g. wipe out all interpreters and
   threads.)

   Locking: as above.

*/

void
Py_Finalize(void)
{
	if (initialized == 0)
		Py_FatalError("Cannot call Py_Finalize: not yet initialized");
	if (initialized == -1)
		Py_FatalError("Cannot call Py_Finalize: already finalized");
	if (initialized != 1)
		Py_FatalError("Cannot call Py_Finalize: unknown initialized stated");

	/* The interpreter is still entirely intact at this point, and the
	 * exit funcs may be relying on that.  In particular, if some thread
	 * or exit func is still waiting to do an import, the import machinery
	 * expects Py_IsInitialized() to return true.  So don't say the
	 * interpreter is uninitialized until after the exit funcs have run.
	 * Note that Threading.py uses an exit func to do a join on all the
	 * threads created thru it, so this also protects pending imports in
	 * the threads created via Threading.
	 */
	/* XXX FIXME this isn't done in a thread-safe fashion.  The
	 * initialized variable itself isn't accessed atomically, but we
	 * also have a race before we change it after checking it. */
	call_py_exitfuncs();
	initialized = -1;

	/* Flush stdout+stderr */
	flush_std_files();

	_PySignal_FiniSigInt();

	finifinalize();

	/* Disable signal handling */
	_PySignal_Fini();

	/* drop module references we saved */
	Py_XDECREF(warnings_module);
	warnings_module = NULL;

	/* Clear type lookup cache */
	PyType_ClearCache();

	/* Collect garbage.  This may call finalizers; it's nice to call these
	 * before all modules are destroyed.
	 * XXX If a __del__ or weakref callback is triggered here, and tries to
	 * XXX import a module, bad things can happen, because Python no
	 * XXX longer believes it's initialized.
	 * XXX     Fatal Python error: Interpreter not initialized (version mismatch?)
	 * XXX is easy to provoke that way.  I've also seen, e.g.,
	 * XXX     Exception exceptions.ImportError: 'No module named sha'
	 * XXX         in <function callback at 0x008F5718> ignored
	 * XXX but I'm unclear on exactly how that one happens.  In any case,
	 * XXX I haven't seen a real-life report of either of these.
	 */
	PyGC_Collect();
#ifdef COUNT_ALLOCS
	/* With COUNT_ALLOCS, it helps to run GC multiple times:
	   each collection might release some types from the type
	   list, so they become garbage. */
	while (PyGC_Collect() > 0)
		/* nothing */;
#endif

	/* Destroy all modules */
	PyImport_Cleanup();

	/* Flush stdout+stderr (again, in case more was printed) */
	/* XXX io.py is long gone at this point */
	//flush_std_files();

	/* Collect final garbage.  This disposes of cycles created by
	 * new-style class definitions, for example.
	 * XXX This is disabled because it caused too many problems.  If
	 * XXX a __del__ or weakref callback triggers here, Python code has
	 * XXX a hard time running, because even the sys module has been
	 * XXX cleared out (sys.stdout is gone, sys.excepthook is gone, etc).
	 * XXX One symptom is a sequence of information-free messages
	 * XXX coming from threads (if a __del__ or callback is invoked,
	 * XXX other threads can execute too, and any exception they encounter
	 * XXX triggers a comedy of errors as subsystem after subsystem
	 * XXX fails to find what it *expects* to find in sys to help report
	 * XXX the exception and consequent unexpected failures).  I've also
	 * XXX seen segfaults then, after adding print statements to the
	 * XXX Python code getting called.
	 */
	PyGC_Collect();

	/* Destroy the database used by _PyImport_{Fixup,Find}Extension */
	_PyImport_Fini();

	/* Debugging stuff */
#ifdef COUNT_ALLOCS
	dump_counts(stdout);
#endif

	PRINT_TOTAL_REFS();

#ifdef Py_TRACE_REFS
	/* Display all objects still alive -- this can invoke arbitrary
	 * __repr__ overrides, so requires a mostly-intact interpreter.
	 * Alas, a lot of stuff may still be alive now that will be cleaned
	 * up later.
	 */
	if (Py_GETENV("PYTHONDUMPREFS"))
		_Py_PrintReferences(stderr);
#endif /* Py_TRACE_REFS */

	_PyState_ClearThreads();

	/* Now we decref the exception classes.  After this point nothing
	   can raise an exception.  That's okay, because each Fini() method
	   below has been checked to make sure no exceptions are ever
	   raised.
	*/
	_PyExc_Fini();

	/* Sundry finalizers */
	PyMethod_Fini();
	PyFrame_Fini();
	PyCFunction_Fini();
	PyTuple_Fini();
	PyList_Fini();
	PyDict_Fini();
	PySet_Fini();
	PyString_Fini();
	PyBytes_Fini();
	PyLong_Fini();
	PyFloat_Fini();
	PyDict_Fini();

	/* Cleanup Unicode implementation */
	_PyUnicode_Fini();

	/* reset file system default encoding */
	if (!Py_HasFileSystemDefaultEncoding) {
		free((char*)Py_FileSystemDefaultEncoding);
        	Py_FileSystemDefaultEncoding = NULL;
	}

	/* XXX Still allocated:
	   - various static ad-hoc pointers to interned strings
	   - int and float free list blocks
	   - whatever various modules and libraries allocate
	*/

	PyGrammar_RemoveAccelerators(&_PyParser_Grammar);

#ifdef Py_TRACE_REFS
	/* Display addresses (& refcnts) of all objects still alive.
	 * An address can be used to find the repr of the object, printed
	 * above by _Py_PrintReferences.
	 */
	if (Py_GETENV("PYTHONDUMPREFS"))
		_Py_PrintReferenceAddresses(stderr);
#endif /* Py_TRACE_REFS */
#ifdef PYMALLOC_DEBUG
	if (Py_GETENV("PYTHONMALLOCSTATS"))
		_PyObject_DebugMallocStats();
#endif

	_PyUnicode_PostFini();

	call_ll_exitfuncs();

	/* Delete current thread */
	PyState_Exit(enterframe);

	/* Cleanup auto-thread-state */
	_PyState_Fini();

	_Py_Refchain_Fini();
}

static char *progname = "python";

void
Py_SetProgramName(char *pn)
{
	if (pn && *pn)
		progname = pn;
}

char *
Py_GetProgramName(void)
{
	return progname;
}

static char *default_home = NULL;

void
Py_SetPythonHome(char *home)
{
	default_home = home;
}

char *
Py_GetPythonHome(void)
{
	char *home = default_home;
	if (home == NULL && !Py_IgnoreEnvironmentFlag)
		home = Py_GETENV("PYTHONHOME");
	return home;
}

/* Create finalizer thread */
static void
initfinalize(void)
{
    PyObject *queue, *runfinalizers, *x;

    queue = PyObject_CallObject((PyObject *)&_PyDeathQueue_Type, NULL);
    if (queue == NULL)
        Py_FatalError("Py_Initialize: can't initialize finalizequeue");
    if (PySys_SetObject("finalizequeue", queue) != 0)
        Py_FatalError("Py_Initialize: can't assign finalizequeue");

    runfinalizers = PySys_GetObject("_runfinalizers");
    if (runfinalizers == NULL)
        Py_FatalError("lost sys._runfinalizers");
    Py_INCREF(runfinalizers);  /* PySys_GetObject returns a borrowed reference */

    finalize_branch = PyObject_CallObject((PyObject *)&PyBranch_Type, NULL);
    if (finalize_branch == NULL)
        Py_FatalError("Failed to initialize finalize_branch");
    x = PyObject_CallMethod(finalize_branch, "__enter__", "");
    if (x == NULL)
        Py_FatalError("Failed to call finalize_branch.__enter__()");
    Py_DECREF(x);

    x = PyObject_CallMethod(finalize_branch, "add", "OO", runfinalizers, queue);
    if (x == NULL)
        Py_FatalError("Failed to spawn finalizer thread");
    Py_DECREF(queue);
    Py_DECREF(runfinalizers);
    Py_DECREF(x);
}

/* Destroy the finalizer thread */
static void
finifinalize(void)
{
    PyObject *x, *dummy, *queue;
    PyObject *e_type, *e_val, *e_tb;

    dummy = PySet_New(NULL);
    if (dummy == NULL)
        Py_FatalError("failed to create dummy to exit finalizer thread");
    queue = PySys_GetObject("finalizequeue");
    if (queue == NULL)
        Py_FatalError("lost sys.finalizequeue");

    x = PyObject_CallMethod(queue, "watch", "OO", dummy, Py_None);
    if (x == NULL)
        Py_FatalError("failed to watch dummy to exit finalizer thread");
    Py_DECREF(x);
    Py_DECREF(dummy);
    //Py_DECREF(queue);  // PySys_GetObject returns a borrowed reference

    /* XXX We're dependant on the GC deleting dummy, and preferably
     * immediately.  This SHOULD happen, but it's not a great assumption. */

    /* XXX This is all a big bodge */
    PyErr_Fetch(&e_type, &e_val, &e_tb);
    PyErr_NormalizeException(&e_type, &e_val, &e_tb);
    if (e_type == NULL) {
        Py_INCREF(Py_None);
        e_type = Py_None;
        Py_INCREF(Py_None);
        e_val = Py_None;
    }
    if (e_tb == NULL) {
        Py_INCREF(Py_None);
        e_tb = Py_None;
    }
    assert(e_type && e_val && e_tb);
    x = PyObject_CallMethod(finalize_branch, "__exit__", "OOO", e_type, e_val, e_tb);
    /* XXX any DECREFs needed? */
    if (x == NULL)
        PyErr_Print();
        //Py_FatalError("failed to call finalize_branch.__exit__()");
    Py_XDECREF(x);
    Py_CLEAR(finalize_branch);
    /* XXX reraise or whatever as required by __exit__ specs */
}

/* Create __main__ module */

static void
initmain(void)
{
	PyObject *m, *d;
	m = PyImport_AddModule("__main__");
	if (m == NULL)
		Py_FatalError("can't create __main__ module");
	d = PyModule_GetDict(m);
	if (PyDict_GetItemString(d, "__builtins__") == NULL) {
		PyObject *bimod = PyImport_ImportModule("builtins");
		if (bimod == NULL ||
		    PyDict_SetItemString(d, "__builtins__", bimod) != 0)
			Py_FatalError("can't add __builtins__ to __main__");
		Py_DECREF(bimod);
	}
}

/* Import the site module (not into __main__ though) */

static void
initsite(void)
{
	PyObject *m, *f;
	m = PyImport_ImportModule("site");
	if (m == NULL) {
		f = PySys_GetObject("stderr");
		if (f == NULL || f == Py_None)
			return;
		if (Py_VerboseFlag) {
			PyFile_WriteString(
				"'import site' failed; traceback:\n", f);
			PyErr_Print();
		}
		else {
			PyFile_WriteString(
			  "'import site' failed; use -v for traceback\n", f);
			PyErr_Clear();
		}
	}
	else {
		Py_DECREF(m);
	}
}

/* Initialize sys.stdin, stdout, stderr and builtins.open */
static int
initstdio(void)
{
	PyObject *iomod = NULL, *wrapper;
	PyObject *bimod = NULL;
	PyObject *m;
	PyObject *std = NULL;
	int status = 0, fd;

	/* Hack to avoid a nasty recursion issue when Python is invoked
	   in verbose mode: pre-import the Latin-1 and UTF-8 codecs */
	if ((m = PyImport_ImportModule("encodings.utf_8")) == NULL) {
		goto error;
	}
	Py_DECREF(m);

	if (!(m = PyImport_ImportModule("encodings.latin_1"))) {
		goto error;
	}
	Py_DECREF(m);

	if (!(bimod = PyImport_ImportModule("builtins"))) {
		goto error;
	}

	if (!(iomod = PyImport_ImportModule("io"))) {
		goto error;
	}
	if (!(wrapper = PyObject_GetAttrString(iomod, "OpenWrapper"))) {
		goto error;
	}

	/* Set builtins.open */
	if (PyObject_SetAttrString(bimod, "open", wrapper) == -1) {
		goto error;
	}

	/* Set sys.stdin */
	fd = fileno(stdin);
	/* Under some conditions stdin, stdout and stderr may not be connected
	 * and fileno() may point to an invalid file descriptor. For example
	 * GUI apps don't have valid standard streams by default.
	 */
	if (fd < 0) {
#ifdef MS_WINDOWS
		std = Py_None;
		Py_INCREF(std);
#else
		goto error;
#endif
	}
	else {
		if (!(std = PyFile_FromFd(fd, "<stdin>", "r", -1, NULL, NULL,
					  "\n", 0))) {
			goto error;
		}
	} /* if (fd < 0) */
	PySys_SetObject("__stdin__", std);
	PySys_SetObject("stdin", std);
	Py_DECREF(std);

	/* Set sys.stdout */
	fd = fileno(stdout);
	if (fd < 0) {
#ifdef MS_WINDOWS
		std = Py_None;
		Py_INCREF(std);
#else
		goto error;
#endif
	}
	else {
		if (!(std = PyFile_FromFd(fd, "<stdout>", "w", -1, NULL, NULL,
					  "\n", 0))) {
			goto error;
		}
	} /* if (fd < 0) */
	PySys_SetObject("__stdout__", std);
	PySys_SetObject("stdout", std);
	Py_DECREF(std);

#if 1 /* Disable this if you have trouble debugging bootstrap stuff */
	/* Set sys.stderr, replaces the preliminary stderr */
	fd = fileno(stderr);
	if (fd < 0) {
#ifdef MS_WINDOWS
		std = Py_None;
		Py_INCREF(std);
#else
		goto error;
#endif
	}
	else {
		if (!(std = PyFile_FromFd(fd, "<stderr>", "w", -1, NULL, NULL,
					  "\n", 0))) {
			goto error;
		}
	} /* if (fd < 0) */
	PySys_SetObject("__stderr__", std);
	PySys_SetObject("stderr", std);
	Py_DECREF(std);
#endif

	if (0) {
  error:
		status = -1;
	}

	Py_XDECREF(bimod);
	Py_XDECREF(iomod);
	return status;
}

/* Parse input from a file and execute it */

int
PyRun_AnyFileExFlags(FILE *fp, const char *filename, int closeit,
		     PyCompilerFlags *flags)
{
	if (filename == NULL)
		filename = "???";
	if (Py_FdIsInteractive(fp, filename)) {
		int err = PyRun_InteractiveLoopFlags(fp, filename, flags);
		if (closeit)
			fclose(fp);
		return err;
	}
	else
		return PyRun_SimpleFileExFlags(fp, filename, closeit, flags);
}

int
PyRun_InteractiveLoopFlags(FILE *fp, const char *filename, PyCompilerFlags *flags)
{
	PyObject *v;
	int ret;
	PyCompilerFlags local_flags;

	if (flags == NULL) {
		flags = &local_flags;
		local_flags.cf_flags = 0;
	}
	v = PySys_GetObject("ps1");
	if (v == NULL) {
		PySys_SetObject("ps1", v = PyUnicode_FromString(">>> "));
		Py_XDECREF(v);
	}
	v = PySys_GetObject("ps2");
	if (v == NULL) {
		PySys_SetObject("ps2", v = PyUnicode_FromString("... "));
		Py_XDECREF(v);
	}
	for (;;) {
		ret = PyRun_InteractiveOneFlags(fp, filename, flags);
		PRINT_TOTAL_REFS();
		if (ret == E_EOF)
			return 0;
		/*
		if (ret == E_NOMEM)
			return -1;
		*/
	}
}

/* compute parser flags based on compiler flags */
#define PARSER_FLAGS(flags) \
	((flags) ? ((((flags)->cf_flags & PyCF_DONT_IMPLY_DEDENT) ? \
		      PyPARSE_DONT_IMPLY_DEDENT : 0)) : 0)

#if 0
/* Keep an example of flags with future keyword support. */
#define PARSER_FLAGS(flags) \
	((flags) ? ((((flags)->cf_flags & PyCF_DONT_IMPLY_DEDENT) ? \
		      PyPARSE_DONT_IMPLY_DEDENT : 0) \
		    | ((flags)->cf_flags & CO_FUTURE_WITH_STATEMENT ? \
		       PyPARSE_WITH_IS_KEYWORD : 0)) : 0)
#endif

int
PyRun_InteractiveOneFlags(FILE *fp, const char *filename, PyCompilerFlags *flags)
{
	PyObject *m, *d, *v, *w, *oenc = NULL;
	mod_ty mod;
	PyArena *arena;
	char *ps1 = "", *ps2 = "", *enc = NULL;
	int errcode = 0;

	if (fp == stdin) {
		/* Fetch encoding from sys.stdin */
		v = PySys_GetObject("stdin");
		if (v == NULL || v == Py_None)
			return -1;
		oenc = PyObject_GetAttrString(v, "encoding");
		if (!oenc)
			return -1;
		enc = PyUnicode_AsString(oenc);
	}
	v = PySys_GetObject("ps1");
	if (v != NULL) {
		v = PyObject_Str(v);
		if (v == NULL)
			PyErr_Clear();
		else if (PyUnicode_Check(v))
			ps1 = PyUnicode_AsString(v);
	}
	w = PySys_GetObject("ps2");
	if (w != NULL) {
		w = PyObject_Str(w);
		if (w == NULL)
			PyErr_Clear();
		else if (PyUnicode_Check(w))
			ps2 = PyUnicode_AsString(w);
	}
	arena = PyArena_New();
	if (arena == NULL) {
		Py_XDECREF(v);
		Py_XDECREF(w);
		Py_XDECREF(oenc);
		return -1;
	}
	mod = PyParser_ASTFromFile(fp, filename, enc,
				   Py_single_input, ps1, ps2,
				   flags, &errcode, arena);
	Py_XDECREF(v);
	Py_XDECREF(w);
	Py_XDECREF(oenc);
	if (mod == NULL) {
		PyArena_Free(arena);
		if (errcode == E_EOF) {
			PyErr_Clear();
			return E_EOF;
		}
		PyErr_Print();
		return -1;
	}
	m = PyImport_AddModule("__main__");
	if (m == NULL) {
		PyArena_Free(arena);
		return -1;
	}
	d = PyModule_GetDict(m);
	v = run_mod(mod, filename, d, d, flags, arena);
	PyArena_Free(arena);
	if (v == NULL) {
		PyErr_Print();
		return -1;
	}
	Py_DECREF(v);
	return 0;
}

/* Check whether a file maybe a pyc file: Look at the extension,
   the file type, and, if we may close it, at the first few bytes. */

static int
maybe_pyc_file(FILE *fp, const char* filename, const char* ext, int closeit)
{
	if (strcmp(ext, ".pyc") == 0 || strcmp(ext, ".pyo") == 0)
		return 1;

	/* Only look into the file if we are allowed to close it, since
	   it then should also be seekable. */
	if (closeit) {
		/* Read only two bytes of the magic. If the file was opened in
		   text mode, the bytes 3 and 4 of the magic (\r\n) might not
		   be read as they are on disk. */
		unsigned int halfmagic = PyImport_GetMagicNumber() & 0xFFFF;
		unsigned char buf[2];
		/* Mess:  In case of -x, the stream is NOT at its start now,
		   and ungetc() was used to push back the first newline,
		   which makes the current stream position formally undefined,
		   and a x-platform nightmare.
		   Unfortunately, we have no direct way to know whether -x
		   was specified.  So we use a terrible hack:  if the current
		   stream position is not 0, we assume -x was specified, and
		   give up.  Bug 132850 on SourceForge spells out the
		   hopelessness of trying anything else (fseek and ftell
		   don't work predictably x-platform for text-mode files).
		*/
		int ispyc = 0;
		if (ftell(fp) == 0) {
			if (fread(buf, 1, 2, fp) == 2 &&
			    ((unsigned int)buf[1]<<8 | buf[0]) == halfmagic)
				ispyc = 1;
			rewind(fp);
		}
		return ispyc;
	}
	return 0;
}

int
PyRun_SimpleFileExFlags(FILE *fp, const char *filename, int closeit,
			PyCompilerFlags *flags)
{
	PyObject *m, *d, *v;
	const char *ext;
	int set_file_name = 0, ret;

	m = PyImport_AddModule("__main__");
	if (m == NULL)
		return -1;
	d = PyModule_GetDict(m);
	if (PyDict_GetItemString(d, "__file__") == NULL) {
		PyObject *f;
		f = PyUnicode_DecodeFSDefault(filename);
		if (f == NULL)
			return -1;
		if (PyDict_SetItemString(d, "__file__", f) < 0) {
			Py_DECREF(f);
			return -1;
		}
		set_file_name = 1;
		Py_DECREF(f);
	}
	ext = filename + strlen(filename) - 4;
	if (maybe_pyc_file(fp, filename, ext, closeit)) {
		/* Try to run a pyc file. First, re-open in binary */
		if (closeit)
			fclose(fp);
		if ((fp = fopen(filename, "rb")) == NULL) {
			fprintf(stderr, "python: Can't reopen .pyc file\n");
			ret = -1;
			goto done;
		}
		/* Turn on optimization if a .pyo file is given */
		if (strcmp(ext, ".pyo") == 0)
			Py_OptimizeFlag = 1;
		v = run_pyc_file(fp, filename, d, d, flags);
	} else {
		v = PyRun_FileExFlags(fp, filename, Py_file_input, d, d,
				      closeit, flags);
	}
	if (v == NULL) {
		PyErr_Print();
		ret = -1;
		goto done;
	}
	Py_DECREF(v);
	ret = 0;
  done:
	if (set_file_name && PyDict_DelItemString(d, "__file__"))
		PyErr_Clear();
	return ret;
}

int
PyRun_SimpleStringFlags(const char *command, PyCompilerFlags *flags)
{
	PyObject *m, *d, *v;
	m = PyImport_AddModule("__main__");
	if (m == NULL)
		return -1;
	d = PyModule_GetDict(m);
	v = PyRun_StringFlags(command, Py_file_input, d, d, flags);
	if (v == NULL) {
		PyErr_Print();
		return -1;
	}
	Py_DECREF(v);
	return 0;
}

static int
parse_syntax_error(PyObject *err, PyObject **message, const char **filename,
		   int *lineno, int *offset, const char **text)
{
	long hold;
	PyObject *v;

	/* old style errors */
	if (PyTuple_Check(err))
		return PyArg_ParseTuple(err, "O(ziiz)", message, filename,
					lineno, offset, text);

	/* new style errors.  `err' is an instance */

	if (! (v = PyObject_GetAttrString(err, "msg")))
		goto finally;
	*message = v;

	if (!(v = PyObject_GetAttrString(err, "filename")))
		goto finally;
	if (v == Py_None)
		*filename = NULL;
	else if (! (*filename = PyUnicode_AsString(v)))
		goto finally;

	Py_DECREF(v);
	if (!(v = PyObject_GetAttrString(err, "lineno")))
		goto finally;
	hold = PyLong_AsLong(v);
	Py_DECREF(v);
	v = NULL;
	if (hold < 0 && PyErr_Occurred())
		goto finally;
	*lineno = (int)hold;

	if (!(v = PyObject_GetAttrString(err, "offset")))
		goto finally;
	if (v == Py_None) {
		*offset = -1;
		Py_DECREF(v);
		v = NULL;
	} else {
		hold = PyLong_AsLong(v);
		Py_DECREF(v);
		v = NULL;
		if (hold < 0 && PyErr_Occurred())
			goto finally;
		*offset = (int)hold;
	}

	if (!(v = PyObject_GetAttrString(err, "text")))
		goto finally;
	if (v == Py_None)
		*text = NULL;
        else if (!PyUnicode_Check(v) ||
		 !(*text = PyUnicode_AsString(v)))
		goto finally;
	Py_DECREF(v);
	return 1;

finally:
	Py_XDECREF(v);
	return 0;
}

void
PyErr_Print(void)
{
	PyErr_PrintEx(1);
}

static void
print_error_text(PyObject *f, int offset, const char *text)
{
	char *nl;
	if (offset >= 0) {
		if (offset > 0 && offset == (int)strlen(text))
			offset--;
		for (;;) {
			nl = strchr(text, '\n');
			if (nl == NULL || nl-text >= offset)
				break;
			offset -= (int)(nl+1-text);
			text = nl+1;
		}
		while (*text == ' ' || *text == '\t') {
			text++;
			offset--;
		}
	}
	PyFile_WriteString("    ", f);
	PyFile_WriteString(text, f);
	if (*text == '\0' || text[strlen(text)-1] != '\n')
		PyFile_WriteString("\n", f);
	if (offset == -1)
		return;
	PyFile_WriteString("    ", f);
	offset--;
	while (offset > 0) {
		PyFile_WriteString(" ", f);
		offset--;
	}
	PyFile_WriteString("^\n", f);
}

static void
handle_system_exit(void)
{
	PyObject *exception, *value, *tb;
	int exitcode = 0;

	if (Py_InspectFlag)
		/* Don't exit if -i flag was given. This flag is set to 0
		 * when entering interactive mode for inspecting. */
		return;

	PyErr_Fetch(&exception, &value, &tb);
	fflush(stdout);
	if (value == NULL || value == Py_None)
		goto done;
	if (PyExceptionInstance_Check(value)) {
		/* The error code should be in the `code' attribute. */
		PyObject *code = PyObject_GetAttrString(value, "code");
		if (code) {
			Py_DECREF(value);
			value = code;
			if (value == Py_None)
				goto done;
		}
		/* If we failed to dig out the 'code' attribute,
		   just let the else clause below print the error. */
	}
	if (PyLong_Check(value))
		exitcode = (int)PyLong_AsLong(value);
	else {
		PyObject_Print(value, stderr, Py_PRINT_RAW);
		PySys_WriteStderr("\n");
		exitcode = 1;
	}
 done:
	/* Restore and clear the exception info, in order to properly decref
	 * the exception, value, and traceback.	 If we just exit instead,
	 * these leak, which confuses PYTHONDUMPREFS output, and may prevent
	 * some finalizers from running.
	 */
	PyErr_Restore(exception, value, tb);
	PyErr_Clear();
	Py_Exit(exitcode);
	/* NOTREACHED */
}

void
PyErr_PrintEx(int set_sys_last_vars)
{
	PyObject *exception, *v, *tb, *hook;

	if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
		handle_system_exit();
	}
	PyErr_Fetch(&exception, &v, &tb);
	if (exception == NULL)
		return;
	PyErr_NormalizeException(&exception, &v, &tb);
	if (exception == NULL)
		return;
        /* Now we know v != NULL too */
#warning XXX FIXME last_value (and friends needs to be redesigned
	/* These can actually cause a recursion loop involving
	 * Py_FatalError and unicode's critical section */
#if 0
	if (set_sys_last_vars) {
		PySys_SetObject("last_type", exception);
		PySys_SetObject("last_value", v);
		PySys_SetObject("last_traceback", tb ? tb : Py_None);
	}
#endif
	hook = PySys_GetObject("excepthook");
	if (hook) {
		PyObject *args = PyTuple_Pack(3,
		    exception, v, tb ? tb : Py_None);
		PyObject *result = PyEval_CallObject(hook, args);
		if (result == NULL) {
			PyObject *exception2, *v2, *tb2;
			if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
				handle_system_exit();
			}
			PyErr_Fetch(&exception2, &v2, &tb2);
			PyErr_NormalizeException(&exception2, &v2, &tb2);
			/* It should not be possible for exception2 or v2
			   to be NULL. However PyErr_Display() can't
			   tolerate NULLs, so just be safe. */
			if (exception2 == NULL) {
				exception2 = Py_None;
				Py_INCREF(exception2);
			}
			if (v2 == NULL) {
				v2 = Py_None;
				Py_INCREF(v2);
			}
			fflush(stdout);
			PySys_WriteStderr("Error in sys.excepthook:\n");
			PyErr_Display(exception2, v2, tb2);
			PySys_WriteStderr("\nOriginal exception was:\n");
			PyErr_Display(exception, v, tb);
			Py_DECREF(exception2);
			Py_DECREF(v2);
			Py_XDECREF(tb2);
		}
		Py_XDECREF(result);
		Py_XDECREF(args);
	} else {
		PySys_WriteStderr("sys.excepthook is missing\n");
		PyErr_Display(exception, v, tb);
	}
	Py_XDECREF(exception);
	Py_XDECREF(v);
	Py_XDECREF(tb);
}

void
PyErr_Display(PyObject *exception, PyObject *value, PyObject *tb)
{
	int err = 0;
	PyObject *f = PySys_GetObject("stderr");
	Py_INCREF(value);
	if (f == Py_None) {
		/* pass */
	}
	else if (f == NULL) {
		_PyObject_Dump(value);
		fprintf(stderr, "lost sys.stderr\n");
	}
	else {
		fflush(stdout);
		if (tb && tb != Py_None)
			err = PyTraceBack_Print(tb, f);
		if (err == 0 &&
		    PyObject_HasAttrString(value, "print_file_and_line"))
		{
			PyObject *message;
			const char *filename, *text;
			int lineno, offset;
			if (!parse_syntax_error(value, &message, &filename,
						&lineno, &offset, &text))
				PyErr_Clear();
			else {
				char buf[10];
				PyFile_WriteString("  File \"", f);
				if (filename == NULL)
					PyFile_WriteString("<string>", f);
				else
					PyFile_WriteString(filename, f);
				PyFile_WriteString("\", line ", f);
				PyOS_snprintf(buf, sizeof(buf), "%d", lineno);
				PyFile_WriteString(buf, f);
				PyFile_WriteString("\n", f);
				if (text != NULL)
					print_error_text(f, offset, text);
				Py_DECREF(value);
				value = message;
				/* Can't be bothered to check all those
				   PyFile_WriteString() calls */
				if (PyErr_Occurred())
					err = -1;
			}
		}
		if (err) {
			/* Don't do anything else */
		}
		else if (PyExceptionClass_Check(exception)) {
			PyObject* moduleName;
			char* className = PyExceptionClass_Name(exception);
			if (className != NULL) {
				char *dot = strrchr(className, '.');
				if (dot != NULL)
					className = dot+1;
			}

			moduleName = PyObject_GetAttrString(exception, "__module__");
			if (moduleName == NULL || !PyUnicode_Check(moduleName))
			{
				Py_DECREF(moduleName);
				err = PyFile_WriteString("<unknown>", f);
			}
			else {
				char* modstr = PyUnicode_AsString(moduleName);
				if (modstr && strcmp(modstr, "builtins"))
				{
					err = PyFile_WriteString(modstr, f);
					err += PyFile_WriteString(".", f);
				}
				Py_DECREF(moduleName);
			}
			if (err == 0) {
				if (className == NULL)
				      err = PyFile_WriteString("<unknown>", f);
				else
				      err = PyFile_WriteString(className, f);
			}
		}
		else
			err = PyFile_WriteObject(exception, f, Py_PRINT_RAW);
		if (err == 0 && (value != Py_None)) {
			PyObject *s = PyObject_Str(value);
			/* only print colon if the str() of the
			   object is not the empty string
			*/
			if (s == NULL)
				err = -1;
			else if (!PyUnicode_Check(s) ||
				 PyUnicode_GetSize(s) != 0)
				err = PyFile_WriteString(": ", f);
			if (err == 0)
			  err = PyFile_WriteObject(s, f, Py_PRINT_RAW);
			Py_XDECREF(s);
		}
		/* try to write a newline in any case */
		err += PyFile_WriteString("\n", f);
	}
	Py_DECREF(value);
	/* If an error happened here, don't show it.
	   XXX This is wrong, but too many callers rely on this behavior. */
	if (err != 0)
		PyErr_Clear();
}

PyObject *
PyRun_StringFlags(const char *str, int start, PyObject *globals,
		  PyObject *locals, PyCompilerFlags *flags)
{
	PyObject *ret = NULL;
	mod_ty mod;
	PyArena *arena = PyArena_New();
	if (arena == NULL)
		return NULL;

	mod = PyParser_ASTFromString(str, "<string>", start, flags, arena);
	if (mod != NULL)
		ret = run_mod(mod, "<string>", globals, locals, flags, arena);
	PyArena_Free(arena);
	return ret;
}

PyObject *
PyRun_FileExFlags(FILE *fp, const char *filename, int start, PyObject *globals,
		  PyObject *locals, int closeit, PyCompilerFlags *flags)
{
	PyObject *ret;
	mod_ty mod;
	PyArena *arena = PyArena_New();
	if (arena == NULL)
		return NULL;

	mod = PyParser_ASTFromFile(fp, filename, NULL, start, 0, 0,
				   flags, NULL, arena);
	if (closeit)
		fclose(fp);
	if (mod == NULL) {
		PyArena_Free(arena);
		return NULL;
	}
	ret = run_mod(mod, filename, globals, locals, flags, arena);
	PyArena_Free(arena);
	return ret;
}

static void
flush_io(void)
{
	PyObject *f, *r;
	f = PySys_GetObject("stderr");
	if (f != NULL) {
		r = PyObject_CallMethod(f, "flush", "");
		if (r)
			Py_DECREF(r);
		else
			PyErr_Clear();
	}
	f = PySys_GetObject("stdout");
	if (f != NULL) {
		r = PyObject_CallMethod(f, "flush", "");
		if (r)
			Py_DECREF(r);
		else
			PyErr_Clear();
	}
}

static PyObject *
run_mod(mod_ty mod, const char *filename, PyObject *globals, PyObject *locals,
	 PyCompilerFlags *flags, PyArena *arena)
{
	PyCodeObject *co;
	PyObject *v;
	co = PyAST_Compile(mod, filename, flags, arena);
	if (co == NULL)
		return NULL;
	v = PyEval_EvalCode(co, globals, locals);
	Py_DECREF(co);
	flush_io();
	return v;
}

static PyObject *
run_pyc_file(FILE *fp, const char *filename, PyObject *globals,
	     PyObject *locals, PyCompilerFlags *flags)
{
	PyCodeObject *co;
	PyObject *v;
	long magic;
	long PyImport_GetMagicNumber(void);

	magic = PyMarshal_ReadLongFromFile(fp);
	if (magic != PyImport_GetMagicNumber()) {
		PyErr_SetString(PyExc_RuntimeError,
			   "Bad magic number in .pyc file");
		return NULL;
	}
	(void) PyMarshal_ReadLongFromFile(fp);
	v = PyMarshal_ReadLastObjectFromFile(fp);
	fclose(fp);
	if (v == NULL || !PyCode_Check(v)) {
		Py_XDECREF(v);
		PyErr_SetString(PyExc_RuntimeError,
			   "Bad code object in .pyc file");
		return NULL;
	}
	co = (PyCodeObject *)v;
	v = PyEval_EvalCode(co, globals, locals);
	if (v && flags)
		flags->cf_flags |= (co->co_flags & PyCF_MASK);
	Py_DECREF(co);
	flush_io();
	return v;
}

PyObject *
Py_CompileStringFlags(const char *str, const char *filename, int start,
		      PyCompilerFlags *flags)
{
	PyCodeObject *co;
	mod_ty mod;
	PyArena *arena = PyArena_New();
	if (arena == NULL)
		return NULL;

	mod = PyParser_ASTFromString(str, filename, start, flags, arena);
	if (mod == NULL) {
		PyArena_Free(arena);
		return NULL;
	}
	if (flags && (flags->cf_flags & PyCF_ONLY_AST)) {
		PyObject *result = PyAST_mod2obj(mod);
		PyArena_Free(arena);
		return result;
	}
	co = PyAST_Compile(mod, filename, flags, arena);
	PyArena_Free(arena);
	return (PyObject *)co;
}

struct symtable *
Py_SymtableString(const char *str, const char *filename, int start)
{
	struct symtable *st;
	mod_ty mod;
	PyArena *arena = PyArena_New();
	if (arena == NULL)
		return NULL;

	mod = PyParser_ASTFromString(str, filename, start, NULL, arena);
	if (mod == NULL) {
		PyArena_Free(arena);
		return NULL;
	}
	st = PySymtable_Build(mod, filename, 0);
	PyArena_Free(arena);
	return st;
}

/* Preferred access to parser is through AST. */
mod_ty
PyParser_ASTFromString(const char *s, const char *filename, int start,
		       PyCompilerFlags *flags, PyArena *arena)
{
	mod_ty mod;
	perrdetail err;
	node *n = PyParser_ParseStringFlagsFilename(s, filename,
					&_PyParser_Grammar, start, &err,
					PARSER_FLAGS(flags));
	if (n) {
		mod = PyAST_FromNode(n, flags, filename, arena);
		PyNode_Free(n);
		return mod;
	}
	else {
		err_input(&err);
		return NULL;
	}
}

mod_ty
PyParser_ASTFromFile(FILE *fp, const char *filename, const char* enc,
		     int start, char *ps1,
		     char *ps2, PyCompilerFlags *flags, int *errcode,
		     PyArena *arena)
{
	mod_ty mod;
	perrdetail err;
	node *n = PyParser_ParseFileFlags(fp, filename, enc,
					  &_PyParser_Grammar,
				start, ps1, ps2, &err, PARSER_FLAGS(flags));
	if (n) {
		mod = PyAST_FromNode(n, flags, filename, arena);
		PyNode_Free(n);
		return mod;
	}
	else {
		err_input(&err);
		if (errcode)
			*errcode = err.error;
		return NULL;
	}
}

/* Simplified interface to parsefile -- return node or set exception */

node *
PyParser_SimpleParseFileFlags(FILE *fp, const char *filename, int start, int flags)
{
	perrdetail err;
	node *n = PyParser_ParseFileFlags(fp, filename, NULL,
					  &_PyParser_Grammar,
					  start, NULL, NULL, &err, flags);
	if (n == NULL)
		err_input(&err);

	return n;
}

/* Simplified interface to parsestring -- return node or set exception */

node *
PyParser_SimpleParseStringFlags(const char *str, int start, int flags)
{
	perrdetail err;
	node *n = PyParser_ParseStringFlags(str, &_PyParser_Grammar,
					    start, &err, flags);
	if (n == NULL)
		err_input(&err);
	return n;
}

node *
PyParser_SimpleParseStringFlagsFilename(const char *str, const char *filename,
					int start, int flags)
{
	perrdetail err;
	node *n = PyParser_ParseStringFlagsFilename(str, filename,
				&_PyParser_Grammar, start, &err, flags);
	if (n == NULL)
		err_input(&err);
	return n;
}

node *
PyParser_SimpleParseStringFilename(const char *str, const char *filename, int start)
{
	return PyParser_SimpleParseStringFlagsFilename(str, filename, start, 0);
}

/* May want to move a more generalized form of this to parsetok.c or
   even parser modules. */

void
PyParser_SetError(perrdetail *err)
{
	err_input(err);
}

/* Set the error appropriate to the given input error code (see errcode.h) */

static void
err_input(perrdetail *err)
{
	PyObject *v, *w, *errtype, *errtext;
	PyObject* u = NULL;
	char *msg = NULL;
	errtype = PyExc_SyntaxError;
	switch (err->error) {
	case E_SYNTAX:
		errtype = PyExc_IndentationError;
		if (err->expected == INDENT)
			msg = "expected an indented block";
		else if (err->token == INDENT)
			msg = "unexpected indent";
		else if (err->token == DEDENT)
			msg = "unexpected unindent";
		else {
			errtype = PyExc_SyntaxError;
			msg = "invalid syntax";
		}
		break;
	case E_TOKEN:
		msg = "invalid token";
		break;
	case E_EOFS:
		msg = "EOF while scanning triple-quoted string";
		break;
	case E_EOLS:
		msg = "EOL while scanning single-quoted string";
		break;
	case E_INTR:
		if (!PyErr_Occurred())
			PyErr_SetNone(PyExc_KeyboardInterrupt);
		return;
	case E_NOMEM:
		PyErr_NoMemory();
		return;
	case E_EOF:
		msg = "unexpected EOF while parsing";
		break;
	case E_TABSPACE:
		errtype = PyExc_TabError;
		msg = "inconsistent use of tabs and spaces in indentation";
		break;
	case E_OVERFLOW:
		msg = "expression too long";
		break;
	case E_DEDENT:
		errtype = PyExc_IndentationError;
		msg = "unindent does not match any outer indentation level";
		break;
	case E_TOODEEP:
		errtype = PyExc_IndentationError;
		msg = "too many levels of indentation";
		break;
	case E_DECODE: {
		PyObject *type, *value, *tb;
		PyErr_Fetch(&type, &value, &tb);
		if (value != NULL) {
			u = PyObject_Str(value);
			if (u != NULL) {
				msg = PyUnicode_AsString(u);
			}
		}
		if (msg == NULL)
			msg = "unknown decode error";
		Py_XDECREF(type);
		Py_XDECREF(value);
		Py_XDECREF(tb);
		break;
	}
	case E_LINECONT:
		msg = "unexpected character after line continuation character";
		break;

	case E_IDENTIFIER:
		msg = "invalid character in identifier";
		break;
	default:
		fprintf(stderr, "error=%d\n", err->error);
		msg = "unknown parsing error";
		break;
	}
	/* err->text may not be UTF-8 in case of decoding errors.
	   Explicitly convert to an object. */
	if (!err->text) {
		errtext = Py_None;
		Py_INCREF(Py_None);
	} else {
		errtext = PyUnicode_DecodeUTF8(err->text, strlen(err->text),
					       "replace");
	}
	v = Py_BuildValue("(ziiN)", err->filename,
			  err->lineno, err->offset, errtext);
	if (err->text != NULL) {
		PyObject_FREE(err->text);
		err->text = NULL;
	}
	w = NULL;
	if (v != NULL)
		w = Py_BuildValue("(sO)", msg, v);
	Py_XDECREF(u);
	Py_XDECREF(v);
	PyErr_SetObject(errtype, w);
	Py_XDECREF(w);
}

/* Print fatal error message and abort */

void
Py_FatalError(const char *msg)
{
	fprintf(stderr, "Fatal Python error: %s\n", msg);
	if (PyErr_Occurred()) {
		PyErr_Print();
	}
#ifdef MS_WINDOWS
	OutputDebugString("Fatal Python error: ");
	OutputDebugString(msg);
	OutputDebugString("\n");
#ifdef _DEBUG
	DebugBreak();
#endif
#endif /* MS_WINDOWS */
	abort();
}

/* Clean up and exit */

#ifdef WITH_THREAD
#include "pythread.h"
#endif

static void (*pyexitfunc)(void) = NULL;
/* For the atexit module. */
void _Py_PyAtExit(void (*func)(void))
{
	pyexitfunc = func;
}

static void
call_py_exitfuncs(void)
{
	if (pyexitfunc == NULL)
		return;

	(*pyexitfunc)();
	PyErr_Clear();
}

#define NEXITFUNCS 32
static void (*exitfuncs[NEXITFUNCS])(void);
static int nexitfuncs = 0;

int Py_AtExit(void (*func)(void))
{
	if (nexitfuncs >= NEXITFUNCS)
		return -1;
	exitfuncs[nexitfuncs++] = func;
	return 0;
}

static void
call_ll_exitfuncs(void)
{
	while (nexitfuncs > 0)
		(*exitfuncs[--nexitfuncs])();

	fflush(stdout);
	fflush(stderr);
}

void
Py_Exit(int sts)
{
	Py_Finalize();

	exit(sts);
}


/*
 * The file descriptor fd is considered ``interactive'' if either
 *   a) isatty(fd) is TRUE, or
 *   b) the -i flag was given, and the filename associated with
 *      the descriptor is NULL or "<stdin>" or "???".
 */
int
Py_FdIsInteractive(FILE *fp, const char *filename)
{
	if (isatty((int)fileno(fp)))
		return 1;
	if (!Py_InteractiveFlag)
		return 0;
	return (filename == NULL) ||
	       (strcmp(filename, "<stdin>") == 0) ||
	       (strcmp(filename, "???") == 0);
}


#if defined(USE_STACKCHECK)
#if defined(WIN32) && defined(_MSC_VER)

/* Stack checking for Microsoft C */

#include <malloc.h>
#include <excpt.h>

/*
 * Return non-zero when we run out of memory on the stack; zero otherwise.
 */
int
PyOS_CheckStack(void)
{
	__try {
		/* alloca throws a stack overflow exception if there's
		   not enough space left on the stack */
		alloca(PYOS_STACK_MARGIN * sizeof(void*));
		return 0;
	} __except (GetExceptionCode() == STATUS_STACK_OVERFLOW ?
		        EXCEPTION_EXECUTE_HANDLER : 
		        EXCEPTION_CONTINUE_SEARCH) {
		int errcode = _resetstkoflw();
		if (errcode)
		{
			Py_FatalError("Could not reset the stack!");
		}
	}
	return 1;
}

#endif /* WIN32 && _MSC_VER */

/* Alternate implementations can be added here... */

#endif /* USE_STACKCHECK */


/* Deprecated C API functions still provided for binary compatiblity */

#undef PyParser_SimpleParseFile
PyAPI_FUNC(node *)
PyParser_SimpleParseFile(FILE *fp, const char *filename, int start)
{
	return PyParser_SimpleParseFileFlags(fp, filename, start, 0);
}

#undef PyParser_SimpleParseString
PyAPI_FUNC(node *)
PyParser_SimpleParseString(const char *str, int start)
{
	return PyParser_SimpleParseStringFlags(str, start, 0);
}

#undef PyRun_AnyFile
PyAPI_FUNC(int)
PyRun_AnyFile(FILE *fp, const char *name)
{
	return PyRun_AnyFileExFlags(fp, name, 0, NULL);
}

#undef PyRun_AnyFileEx
PyAPI_FUNC(int)
PyRun_AnyFileEx(FILE *fp, const char *name, int closeit)
{
	return PyRun_AnyFileExFlags(fp, name, closeit, NULL);
}

#undef PyRun_AnyFileFlags
PyAPI_FUNC(int)
PyRun_AnyFileFlags(FILE *fp, const char *name, PyCompilerFlags *flags)
{
	return PyRun_AnyFileExFlags(fp, name, 0, flags);
}

#undef PyRun_File
PyAPI_FUNC(PyObject *)
PyRun_File(FILE *fp, const char *p, int s, PyObject *g, PyObject *l)
{
        return PyRun_FileExFlags(fp, p, s, g, l, 0, NULL);
}

#undef PyRun_FileEx
PyAPI_FUNC(PyObject *)
PyRun_FileEx(FILE *fp, const char *p, int s, PyObject *g, PyObject *l, int c)
{
        return PyRun_FileExFlags(fp, p, s, g, l, c, NULL);
}

#undef PyRun_FileFlags
PyAPI_FUNC(PyObject *)
PyRun_FileFlags(FILE *fp, const char *p, int s, PyObject *g, PyObject *l,
		PyCompilerFlags *flags)
{
        return PyRun_FileExFlags(fp, p, s, g, l, 0, flags);
}

#undef PyRun_SimpleFile
PyAPI_FUNC(int)
PyRun_SimpleFile(FILE *f, const char *p)
{
	return PyRun_SimpleFileExFlags(f, p, 0, NULL);
}

#undef PyRun_SimpleFileEx
PyAPI_FUNC(int)
PyRun_SimpleFileEx(FILE *f, const char *p, int c)
{
	return PyRun_SimpleFileExFlags(f, p, c, NULL);
}


#undef PyRun_String
PyAPI_FUNC(PyObject *)
PyRun_String(const char *str, int s, PyObject *g, PyObject *l)
{
	return PyRun_StringFlags(str, s, g, l, NULL);
}

#undef PyRun_SimpleString
PyAPI_FUNC(int)
PyRun_SimpleString(const char *s)
{
	return PyRun_SimpleStringFlags(s, NULL);
}

#undef Py_CompileString
PyAPI_FUNC(PyObject *)
Py_CompileString(const char *str, const char *p, int s)
{
	return Py_CompileStringFlags(str, p, s, NULL);
}

#undef PyRun_InteractiveOne
PyAPI_FUNC(int)
PyRun_InteractiveOne(FILE *f, const char *p)
{
	return PyRun_InteractiveOneFlags(f, p, NULL);
}

#undef PyRun_InteractiveLoop
PyAPI_FUNC(int)
PyRun_InteractiveLoop(FILE *f, const char *p)
{
	return PyRun_InteractiveLoopFlags(f, p, NULL);
}

#ifdef __cplusplus
}
#endif
