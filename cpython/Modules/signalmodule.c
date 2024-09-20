/* Signal module -- many thanks to Lance Ellinghaus
 * Significantly rewritten by Adam Olsen, to use a dedicated signal thread */

#include "Python.h"

#include "branchobject.h"

#ifdef MS_WINDOWS
#include <process.h>
#endif

#include <signal.h>

#ifndef SIG_ERR
#define SIG_ERR ((PyOS_sighandler_t)(-1))
#endif

#if defined(PYOS_OS2) && !defined(PYCC_GCC)
#define NSIG 12
#include <process.h>
#endif

#ifndef NSIG
# if defined(_NSIG)
#  define NSIG _NSIG            /* For BSD/SysV */
# elif defined(_SIGMAX)
#  define NSIG (_SIGMAX + 1)    /* For QNX */
# elif defined(SIGMAX)
#  define NSIG (SIGMAX + 1)     /* For djgpp */
# else
#  define NSIG 64               /* Use a reasonable default value */
# endif
#endif

#define _PySIGNAL_WAKEUP SIGUSR2


/*
 * NOTES ON THE INTERACTION BETWEEN SIGNALS AND THREADS
 *
 * There isn't much, really.  A dedicated thread now asks what signals
 * are pending, and the real signal handlers are never invoked.  This
 * means the various syscalls will NOT get interrupted (unless something
 * else interferes.)
 *
 * This is less portable though.  In old LinuxThread (linux kernels 2.0
 * and 2.1) the SIGUSR1 and SIGUSR2 signals are unavailable, but we need
 * one to do our own wakeups.  Signal handling with LinuxThreads is
 * broken anyway though, as most signals will only be pending on the
 * main thread, not the signal thread (meaning they'll never get
 * processed.)
 */

#include "pythread.h"
static PyObject *signal_branch;
static PyThread_type_handle signal_branch_handle;
static int signal_branch_waiting;
static int signal_branch_wakeup_sent;
static int signal_branch_reload;
static int signal_branch_quit;
static PyCritical *signal_branch_crit;
static PyObject *sigint_branch;
static PyObject *old_sigint_handler;
static PyMethodDef kbdint_raiser_method_def;

static struct {
    int banned;  /* We won't touch this signal */
    int old_ignored;  /* When python initialized the signal was SIG_IGN (not SIG_DFL) */
    int watched;  /* signal_waiter is watching for this signal */
    PyObject *func;
} Handlers[NSIG];

static PyObject *DefaultHandler;
static PyObject *IgnoreHandler;
static PyObject *IntHandler;


static PyObject *
signal_default_int_handler(PyObject *self, PyObject *args)
{
    PyObject *kbdint_raiser_method, *x;
    PyObject *m = PyImport_ImportModule("signal");
    if (m == NULL)
        return NULL;

    kbdint_raiser_method = PyCFunction_NewEx(&kbdint_raiser_method_def, m, m);
    if (kbdint_raiser_method == NULL)
        return NULL;

    x = PyObject_CallMethod(sigint_branch, "add", "O", kbdint_raiser_method);
    Py_DECREF(kbdint_raiser_method);
    if (x == NULL)
        return NULL;
    Py_DECREF(x);

    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(default_int_handler_doc,
"default_int_handler(...)\n\
\n\
The default handler for SIGINT installed by Python.\n\
It causes KeyboardInterrupt to be raised from a hidden branch above\n\
the __main__ module.");


static PyObject *
kbdint_raiser(PyObject *self)
{
    PyErr_SetNone(PyExc_KeyboardInterrupt);
    return NULL;
}

static void
dummy(int sig_num)
{
    Py_FatalError("dummy should never be called");
}

static PyObject *
signal_waiter(PyObject *self)
{
    sigset_t set, wakeupset;
    int sig, wakeupsig;
    int i;
    PyObject *func, *x;
    PyObject *retval = NULL;

    sigemptyset(&wakeupset);
    sigaddset(&wakeupset, _PySIGNAL_WAKEUP);

    PyCritical_Enter(signal_branch_crit);
    signal_branch_handle = PyThread_get_handle();

    PyOS_setsig(_PySIGNAL_WAKEUP, dummy);

    while (1) {
        if (signal_branch_reload) {
            /* Stop watching old signals */
            sigemptyset(&set);
            for (i = 1; i < NSIG; i++) {
                if (Handlers[i].banned)
                    continue;

                if (Handlers[i].watched &&
                        (Handlers[i].func == DefaultHandler ||
                        Handlers[i].func == IgnoreHandler)) {
                    void (*func)(int);
                    if (Handlers[i].func == IgnoreHandler)
                        func = SIG_IGN;
                    else
                        func = SIG_DFL;

                    PyOS_setsig(i, func);
                    sigaddset(&set, i);
                    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
                    Handlers[i].watched = 0;
                }
            }

            /* Start watching new signals.  Also sets the set for sigwait */
            sigemptyset(&set);
            for (i = 1; i < NSIG; i++) {
                if (Handlers[i].banned)
                    continue;

                if (!Handlers[i].watched &&
                        Handlers[i].func != DefaultHandler &&
                        Handlers[i].func != IgnoreHandler) {
                    sigaddset(&set, i);
                    pthread_sigmask(SIG_BLOCK, &set, NULL);
                    PyOS_setsig(i, dummy);
                    Handlers[i].watched = 1;
                } else if (Handlers[i].watched)
                    sigaddset(&set, i);
            }

            /* We always watch for _PySIGNAL_WAKEUP, even though we only
             * block it around sigwait */
            sigaddset(&set, _PySIGNAL_WAKEUP);
            signal_branch_reload = 0;
        }

        if (signal_branch_quit) {
            Py_INCREF(Py_None);
            retval = Py_None;
            break;
        }

        pthread_sigmask(SIG_BLOCK, &wakeupset, NULL);
        signal_branch_waiting = 1;
        PyCritical_Exit(signal_branch_crit);
        PyState_Suspend();

        errno = 0;
        if (sigwait(&set, &sig)) {
            /* XXX I've seen some mention that gdb can cause sigwait to
             * be interrupted.  If verified, this needs to be modified
             * to loop on EINTR */
            dprintf(2, "sigwait failed: %d\n", errno);
            Py_FatalError("sigwait failed!");
        }

        PyState_Resume();
        PyCritical_Enter(signal_branch_crit);
        signal_branch_waiting = 0;
        if (sig != _PySIGNAL_WAKEUP && signal_branch_wakeup_sent) {
            /* Purge any pending wakeups */
            sigwait(&wakeupset, &wakeupsig);
            assert(wakeupsig == _PySIGNAL_WAKEUP);
        }
        signal_branch_wakeup_sent = 0;
        pthread_sigmask(SIG_UNBLOCK, &wakeupset, NULL);

        assert(sig >= 1 && sig < NSIG);
        if (sig != _PySIGNAL_WAKEUP) {
            func = Handlers[sig].func;
            Py_INCREF(func);
            if (func == DefaultHandler || func == IgnoreHandler)
                /* XXX FIXME the signal handler could be changed at the
                 * same time we get a signal for it, meaning we
                 * legitimately have no handler by the time we get here */
                Py_FatalError("sigwait got signal without handler");
            PyCritical_Exit(signal_branch_crit);
            x = PyObject_CallFunction(func, "iO", sig, Py_None);
            Py_DECREF(func);
            Py_XDECREF(x);
            PyCritical_Enter(signal_branch_crit);
            if (x == NULL)
                break;
        }
    }

    /* Reset all signals to their original handlers */
    sigemptyset(&set);
    for (i = 1; i < NSIG; i++) {
        if (!Handlers[i].banned) {
            if (!Handlers[i].old_ignored)
                PyOS_setsig(i, SIG_DFL);
            else
                PyOS_setsig(i, SIG_IGN);
            sigaddset(&set, i);
            Handlers[i].watched = 0;
        }
    }
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    PyOS_setsig(_PySIGNAL_WAKEUP, SIG_DFL);
    PyCritical_Exit(signal_branch_crit);
    return retval;
}


#ifdef HAVE_ALARM
static PyObject *
signal_alarm(PyObject *self, PyObject *args)
{
    int t;
    if (!PyArg_ParseTuple(args, "i:alarm", &t))
        return NULL;
    /* alarm() returns the number of seconds remaining */
    return PyInt_FromLong((long)alarm(t));
}

PyDoc_STRVAR(alarm_doc,
"alarm(seconds)\n\
\n\
Arrange for SIGALRM to arrive after the given number of seconds.");
#endif


static PyObject *
signal_signal(PyObject *self, PyObject *args)
{
    PyObject *obj;
    int sig_num;
    PyObject *old_handler;
    void (*func)(int);

    if (!PyArg_ParseTuple(args, "iO:signal", &sig_num, &obj))
        return NULL;

    if (sig_num < 1 || sig_num >= NSIG) {
        PyErr_SetString(PyExc_ValueError, "signal number out of range");
        return NULL;
    }
    if (obj != IgnoreHandler && obj != DefaultHandler &&
            !PyCallable_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "signal handler must be "
            "signal.SIG_IGN, signal.SIG_DFL, or a callable object");
        return NULL;
    }

    PyCritical_Enter(signal_branch_crit);
    Py_INCREF(obj);
    old_handler = Handlers[sig_num].func;
    Handlers[sig_num].func = obj;
    signal_branch_reload = 1;
    if (signal_branch_waiting && !signal_branch_wakeup_sent) {
        PyThread_send_signal(signal_branch_handle, _PySIGNAL_WAKEUP);
        signal_branch_wakeup_sent = 1;
    }
    PyCritical_Exit(signal_branch_crit);

    return old_handler;
}

PyDoc_STRVAR(signal_doc,
"signal(sig, action) -> action\n\
\n\
Set the action for the given signal.  The action can be SIG_DFL,\n\
SIG_IGN, or a callable Python object.  The previous action is\n\
returned.  See getsignal() for possible return values.\n\
\n\
*** IMPORTANT NOTICE ***\n\
A signal handler function is called with only *one* argument: the signal number.");


static PyObject *
signal_getsignal(PyObject *self, PyObject *args)
{
    int sig_num;
    PyObject *old_handler;

    if (!PyArg_ParseTuple(args, "i:getsignal", &sig_num))
        return NULL;

    if (sig_num < 1 || sig_num >= NSIG) {
        PyErr_SetString(PyExc_ValueError, "signal number out of range");
        return NULL;
    }

    PyCritical_Enter(signal_branch_crit);
    old_handler = Handlers[sig_num].func;
    Py_INCREF(old_handler);
    PyCritical_Exit(signal_branch_crit);
    return old_handler;
}

PyDoc_STRVAR(getsignal_doc,
"getsignal(sig) -> action\n\
\n\
Return the current action for the given signal.  The return value can be:\n\
SIG_IGN -- if the signal is being ignored\n\
SIG_DFL -- if the default action for the signal is in effect\n\
None -- if an unknown handler is in effect\n\
anything else -- the callable Python object used as a handler");


/* List of functions defined in the module */
static PyMethodDef signal_methods[] = {
#ifdef HAVE_ALARM
    {"alarm",               signal_alarm, METH_VARARGS, alarm_doc},
#endif
    {"signal",              signal_signal, METH_VARARGS, signal_doc},
    {"getsignal",           signal_getsignal, METH_VARARGS, getsignal_doc},
    {"default_int_handler", signal_default_int_handler, METH_VARARGS,
        default_int_handler_doc},
    {NULL,                  NULL}  /* sentinel */
};

/* This are NOT exported methods.  They are only used internally
 * by the signal machinery */
static PyMethodDef signal_waiter_method_def = {"signal_waiter",
    (PyCFunction)signal_waiter, METH_NOARGS | METH_SHARED, NULL};
static PyMethodDef kbdint_raiser_method_def = {"kbdint_raiser",
    (PyCFunction)kbdint_raiser, METH_NOARGS | METH_SHARED, NULL};


PyDoc_STRVAR(module_doc,
"This module provides mechanisms to use signal handlers in Python.\n\
\n\
Functions:\n\
\n\
alarm() -- cause SIGALRM after a specified time [Unix only]\n\
signal() -- set the action for a given signal\n\
getsignal() -- get the signal action for a given signal\n\
default_int_handler() -- default SIGINT handler\n\
\n\
Constants:\n\
\n\
SIG_DFL -- used to refer to the system default handler\n\
SIG_IGN -- used to ignore the signal\n\
NSIG -- number of defined signals\n\
\n\
SIGINT, SIGTERM, etc. -- signal numbers\n\
\n\
*** IMPORTANT NOTICE ***\n\
A signal handler function is called with two arguments:\n\
the first is the signal number, the second is the interrupted stack frame.");

void
_PySignal_Init(void)
{
    sigset_t set;
    PyObject *m, *d, *x;
    PyObject *e_type, *e_val, *e_tb;
    PyObject *signal_waiter_method;
    int i;

    /* Create the module and add the functions */
    m = Py_InitModule3("signal", signal_methods, module_doc);
    if (m == NULL)
            Py_FatalError("failed to initialize signalmodule");
    /* XXX FIXME is fixup redundant here? */
    _PyImport_FixupExtension("signal", "signal");

    /* Add some symbolic constants to the module */
    d = PyModule_GetDict(m);

    DefaultHandler = PyLong_FromVoidPtr((void *)SIG_DFL);
    if (DefaultHandler == NULL ||
            PyDict_SetItemString(d, "SIG_DFL", DefaultHandler) < 0)
        Py_FatalError("failed to initialize SIG_DFL");

    IgnoreHandler = PyLong_FromVoidPtr((void *)SIG_IGN);
    if (IgnoreHandler == NULL ||
            PyDict_SetItemString(d, "SIG_IGN", IgnoreHandler) < 0)
        Py_FatalError("failed to initialize SIG_IGN");

    x = PyInt_FromLong((long)NSIG);
    if (x == NULL || PyDict_SetItemString(d, "NSIG", x) < 0)
        Py_FatalError("failed to initialize NSIG");
    Py_DECREF(x);

    IntHandler = PyDict_GetItemString(d, "default_int_handler");
    if (IntHandler == NULL)
        Py_FatalError("failed to initialize default_int_handler");
    Py_INCREF(IntHandler);


#define add_signal(name) \
    x = PyInt_FromLong(name); \
    if (x == NULL) \
        Py_FatalError("failed to create signal constant " #name); \
    if (PyDict_SetItemString(d, #name, x) < 0) \
        Py_FatalError("failed to add signal constant " #name); \
    Py_DECREF(x);

#ifdef SIGHUP
    add_signal(SIGHUP);
#endif
#ifdef SIGINT
    add_signal(SIGINT);
#endif
#ifdef SIGBREAK
    add_signal(SIGBREAK);
#endif
#ifdef SIGQUIT
    add_signal(SIGQUIT);
#endif
#ifdef SIGILL
    add_signal(SIGILL);
#endif
#ifdef SIGTRAP
    add_signal(SIGTRAP);
#endif
#ifdef SIGIOT
    add_signal(SIGIOT);
#endif
#ifdef SIGABRT
    add_signal(SIGABRT);
#endif
#ifdef SIGEMT
    add_signal(SIGEMT);
#endif
#ifdef SIGFPE
    add_signal(SIGFPE);
#endif
#ifdef SIGKILL
    add_signal(SIGKILL);
#endif
#ifdef SIGBUS
    add_signal(SIGBUS);
#endif
#ifdef SIGSEGV
    add_signal(SIGSEGV);
#endif
#ifdef SIGSYS
    add_signal(SIGSYS);
#endif
#ifdef SIGPIPE
    add_signal(SIGPIPE);
#endif
#ifdef SIGALRM
    add_signal(SIGALRM);
#endif
#ifdef SIGTERM
    add_signal(SIGTERM);
#endif
#ifdef SIGUSR1
    add_signal(SIGUSR1);
#endif
#ifdef SIGUSR2
    add_signal(SIGUSR2);
#endif
#ifdef SIGCLD
    add_signal(SIGCLD);
#endif
#ifdef SIGCHLD
    add_signal(SIGCHLD);
#endif
#ifdef SIGPWR
    add_signal(SIGPWR);
#endif
#ifdef SIGIO
    add_signal(SIGIO);
#endif
#ifdef SIGURG
    add_signal(SIGURG);
#endif
#ifdef SIGWINCH
    add_signal(SIGWINCH);
#endif
#ifdef SIGPOLL
    add_signal(SIGPOLL);
#endif
#ifdef SIGSTOP
    add_signal(SIGSTOP);
#endif
#ifdef SIGTSTP
    add_signal(SIGTSTP);
#endif
#ifdef SIGCONT
    add_signal(SIGCONT);
#endif
#ifdef SIGTTIN
    add_signal(SIGTTIN);
#endif
#ifdef SIGTTOU
    add_signal(SIGTTOU);
#endif
#ifdef SIGVTALRM
    add_signal(SIGVTALRM);
#endif
#ifdef SIGPROF
    add_signal(SIGPROF);
#endif
#ifdef SIGXCPU
    add_signal(SIGXCPU);
#endif
#ifdef SIGXFSZ
    add_signal(SIGXFSZ);
#endif
#ifdef SIGRTMIN
    add_signal(SIGRTMIN);
#endif
#ifdef SIGRTMAX
    add_signal(SIGRTMAX);
#endif
#ifdef SIGINFO
    add_signal(SIGINFO);
#endif


#ifdef SIGPIPE
    PyOS_setsig(SIGPIPE, SIG_IGN);
#endif
#ifdef SIGXFZ
    PyOS_setsig(SIGXFZ, SIG_IGN);
#endif
#ifdef SIGXFSZ
    PyOS_setsig(SIGXFSZ, SIG_IGN);
#endif


    sigemptyset(&set);
    sigaddset(&set, i);
    for (i = 1; i < NSIG; i++) {
        void (*t)(int);
        t = PyOS_getsig(i);
        if (t == SIG_DFL) {
            Handlers[i].watched = 1;
            Handlers[i].old_ignored = 0;
            Handlers[i].func = DefaultHandler;
            sigaddset(&set, i);
        } else if (t == SIG_IGN) {
            Handlers[i].watched = 1;
            Handlers[i].old_ignored = 1;
            Handlers[i].func = IgnoreHandler;
            sigaddset(&set, i);
        } else {
            Handlers[i].banned = 1; /* None of our business */
            Handlers[i].func = Py_None;
        }
        Py_INCREF(Handlers[i].func);
    }
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    signal_branch_crit = PyCritical_Allocate(PyCRITICAL_NORMAL);
    if (signal_branch_crit == NULL)
        Py_FatalError("failed to initialize signal_branch_crit");

    signal_branch = PyObject_CallObject((PyObject *)&PyBranch_Type, NULL);
    if (signal_branch == NULL)
        Py_FatalError("failed to initialize signal_branch");
    x = PyObject_CallMethod(signal_branch, "__enter__", "");
    if (x == NULL)
        Py_FatalError("failed to call signal_branch.__enter__()");
    Py_DECREF(x);

    signal_branch_reload = 1;
    signal_waiter_method = PyCFunction_NewEx(&signal_waiter_method_def, m, m);
    if (signal_waiter_method == NULL)
        Py_FatalError("failed to create signal_waiter_method");
    x = PyObject_CallMethod(signal_branch, "add", "O", signal_waiter_method);
    Py_DECREF(signal_waiter_method);
    if (x == NULL)
        Py_FatalError("failed to call signal_branch.add()");
    Py_DECREF(x);
}

void
_PySignal_Fini(void)
{
    int i;
    PyObject *x, *func;
    PyObject *e_type, *e_val, *e_tb;

    PyCritical_Enter(signal_branch_crit);
    signal_branch_quit = 1;
    if (signal_branch_waiting && !signal_branch_wakeup_sent) {
        PyThread_send_signal(signal_branch_handle, _PySIGNAL_WAKEUP);
        signal_branch_wakeup_sent = 1;
    }
    PyCritical_Exit(signal_branch_crit);

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
    x = PyObject_CallMethod(signal_branch, "__exit__", "OOO", e_type, e_val, e_tb);
    /* XXX any DECREFs needed? */
    if (x == NULL)
        Py_FatalError("failed to call signal_branch.__exit__()");
    Py_DECREF(x);
    Py_CLEAR(signal_branch);
    /* XXX reraise or whatever as required by __exit__ specs */

    PyCritical_Free(signal_branch_crit);
    signal_branch_crit = NULL;

    for (i = 1; i < NSIG; i++)
        Py_CLEAR(Handlers[i].func);
    Py_CLEAR(IntHandler);
    Py_CLEAR(DefaultHandler);
    Py_CLEAR(IgnoreHandler);
}

void
_PySignal_InitSigInt(int handle_sigint)
{
    PyObject *x;

    sigint_branch = PyObject_CallObject((PyObject *)&PyBranch_Type, NULL);
    if (sigint_branch == NULL)
        Py_FatalError("failed to initialize sigint_branch");
    x = PyObject_CallMethod(sigint_branch, "__enter__", "");
    if (x == NULL)
        Py_FatalError("failed to call sigint_branch.__enter__()");
    Py_DECREF(x);

    if (handle_sigint) {
        PyObject *m = PyImport_ImportModule("signal");
        if (!m)
            Py_FatalError("Can't import signal module");

        old_sigint_handler = PyObject_CallMethod(m, "signal", "iO", SIGINT, IntHandler);
        if (old_sigint_handler == NULL)
            Py_FatalError("Failed to install sigint handler");
    }
}

void
_PySignal_FiniSigInt(void)
{
    PyObject *x;
    PyObject *e_type, *e_val, *e_tb;

    if (old_sigint_handler) {
        PyObject *m = PyImport_ImportModule("signal");
        if (!m)
            Py_FatalError("Can't import signal module");

        x = PyObject_CallMethod(m, "signal", "iO", SIGINT, old_sigint_handler);
        if (x == NULL)
            Py_FatalError("Failed to uninstall sigint handler");
        Py_DECREF(x);
        Py_CLEAR(old_sigint_handler);
    }

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
    x = PyObject_CallMethod(sigint_branch, "__exit__", "OOO", e_type, e_val, e_tb);
    /* XXX any DECREFs needed? */
    if (x == NULL)
        PyErr_Print();
        //Py_FatalError("failed to call sigint_branch.__exit__()");
    Py_XDECREF(x);
    Py_CLEAR(sigint_branch);
    /* XXX reraise or whatever as required by __exit__ specs */
}


/* Wrappers around sigaction() or signal(). */
PyOS_sighandler_t
PyOS_getsig(int sig)
{
#ifdef HAVE_SIGACTION
    struct sigaction context;
    if (sigaction(sig, NULL, &context) == -1)
        return SIG_ERR;
    return context.sa_handler;
#else
    PyOS_sighandler_t handler;
/* Special signal handling for the secure CRT in Visual Studio 2005 */
#if defined(_MSC_VER) && _MSC_VER >= 1400
    switch (sig) {
        /* Only these signals are valid */
        case SIGINT:
        case SIGILL:
        case SIGFPE:
        case SIGSEGV:
        case SIGTERM:
        case SIGBREAK:
        case SIGABRT:
            break;
        /* Don't call signal() with other values or it will assert */
        default:
            return SIG_ERR;
    }
#endif /* _MSC_VER && _MSC_VER >= 1400 */
    handler = signal(sig, SIG_IGN);
    if (handler != SIG_ERR)
        signal(sig, handler);
    return handler;
#endif
}

PyOS_sighandler_t
PyOS_setsig(int sig, PyOS_sighandler_t handler)
{
#ifdef HAVE_SIGACTION
    struct sigaction context, ocontext;
    context.sa_handler = handler;
    sigemptyset(&context.sa_mask);
    context.sa_flags = 0;
    if (sigaction(sig, &context, &ocontext) == -1)
            return SIG_ERR;
    return ocontext.sa_handler;
#else
    PyOS_sighandler_t oldhandler;
    oldhandler = signal(sig, handler);
#ifdef HAVE_SIGINTERRUPT
    siginterrupt(sig, 1);
#endif
    return oldhandler;
#endif
}


void
PyOS_AfterFork(void)
{
    PyState_CleanupForkChild();
    _PyImport_ReInitLock();
}
