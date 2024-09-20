/* Generator object implementation */

#include "Python.h"
#include "frameobject.h"
#include "genobject.h"
#include "ceval.h"
#include "structmember.h"
#include "opcode.h"

static int
gen_traverse(PyGenObject *gen, visitproc visit, void *arg)
{
	Py_VISIT((PyObject *)gen->gi_frame);
	return 0;
}

static inline int
bitcount(AO_t num)
{
	int count = 0;
	while (num) {
		count += num & 1;
		num >>= 1;
	}
	return count;
}

static void
gen_dealloc(PyGenObject *gen)
{
	_PyObject_GC_UNTRACK(gen);

	if (gen->gi_frame != NULL && gen->gi_frame->f_stacktop != NULL) {
		/* XXX We should require all generators with "try"
		 * blocks to be bound to the stack before first running
		 * them, making this impossible. */
		static AO_t warning_count = 0;
		AO_t current = AO_fetch_and_add1_full(&warning_count) + 1;

		/* This might get printed out of order, but who cares?
		 * Heck, there's no guarantee the printing itself is
		 * atomic. */
		/* All we do here is check for powers of 2.  The key to
		 * this test is that the only time your mask won't
		 * include your most-significant-bit is when it rolls
		 * over from 0x100 to 0xFF. */
		if (current != 0 && !(current & (current - 1)))
			fprintf(stderr, "Warning: deleting paused "
				"generator without closing, %lu times\n",
				current);
	}

	Py_CLEAR(gen->gi_frame);
	PyObject_DEL(gen);
}


static PyObject *
gen_send_ex(PyGenObject *gen, PyObject *arg, int exc)
{
	PyThreadState *tstate = PyThreadState_Get();
	PyFrameObject *f = gen->gi_frame;
	PyObject *result;

	if (gen->gi_running) {
		PyErr_SetString(PyExc_ValueError,
				"generator already executing");
		return NULL;
	}
	if (f==NULL || f->f_stacktop == NULL) {
		/* Only set exception if called from send() */
		if (arg && !exc)
			PyErr_SetNone(PyExc_StopIteration);
		return NULL;
	}

	if (f->f_lasti == -1) {
		if (arg && arg != Py_None) {
			PyErr_SetString(PyExc_TypeError,
					"can't send non-None value to a "
					"just-started generator");
			return NULL;
		}
	} else {
		/* Push arg onto the frame's value stack */
		result = arg ? arg : Py_None;
	        Py_INCREF(result);
	        *(f->f_stacktop++) = result;
	}

	/* Generators always return to their most recent caller, not
	 * necessarily their creator. */
	Py_XINCREF(tstate->frame);
	assert(f->f_back == NULL);
	f->f_back = tstate->frame;

	gen->gi_running = 1;
	result = PyEval_EvalFrameEx(f, exc);
	gen->gi_running = 0;

	/* Don't keep the reference to f_back any longer than necessary.  It
	 * may keep a chain of frames alive or it could create a reference
	 * cycle. */
	assert(f->f_back == tstate->frame);
	Py_CLEAR(f->f_back);

	/* If the generator just returned (as opposed to yielding), signal
	 * that the generator is exhausted. */
	if (result == Py_None && f->f_stacktop == NULL) {
		Py_DECREF(result);
		result = NULL;
		/* Set exception if not called by gen_iternext() */
		if (arg)
			PyErr_SetNone(PyExc_StopIteration);
	}

	if (!result || f->f_stacktop == NULL) {
		/* generator can't be rerun, so release the frame */
		Py_DECREF(f);
		gen->gi_frame = NULL;
	}

	return result;
}

PyDoc_STRVAR(send_doc,
"send(arg) -> send 'arg' into generator,\n\
return next yielded value or raise StopIteration.");

static PyObject *
gen_send(PyGenObject *gen, PyObject *arg)
{
	return gen_send_ex(gen, arg, 0);
}

PyDoc_STRVAR(close_doc,
"close(arg) -> raise GeneratorExit inside generator.");

static PyObject *
gen_close(PyGenObject *gen, PyObject *args)
{
	PyObject *retval;
	PyErr_SetNone(PyExc_GeneratorExit);
	retval = gen_send_ex(gen, Py_None, 1);
	if (retval) {
		Py_DECREF(retval);
		PyErr_SetString(PyExc_RuntimeError,
				"generator ignored GeneratorExit");
		return NULL;
	}
	if (PyErr_ExceptionMatches(PyExc_StopIteration)
	    || PyErr_ExceptionMatches(PyExc_GeneratorExit))
	{
		PyErr_Clear();	/* ignore these errors */
		Py_INCREF(Py_None);
		return Py_None;
	}
	return NULL;
}



PyDoc_STRVAR(throw_doc,
"throw(typ[,val[,tb]]) -> raise exception in generator,\n\
return next yielded value or raise StopIteration.");

static PyObject *
gen_throw(PyGenObject *gen, PyObject *args)
{
	PyObject *typ;
	PyObject *tb = NULL;
	PyObject *val = NULL;

	if (!PyArg_UnpackTuple(args, "throw", 1, 3, &typ, &val, &tb))
		return NULL;

	/* First, check the traceback argument, replacing None with
	   NULL. */
	if (tb == Py_None)
		tb = NULL;
	else if (tb != NULL && !PyTraceBack_Check(tb)) {
		PyErr_SetString(PyExc_TypeError,
			"throw() third argument must be a traceback object");
		return NULL;
	}

	Py_INCREF(typ);
	Py_XINCREF(val);
	Py_XINCREF(tb);

	if (PyExceptionClass_Check(typ)) {
		PyErr_NormalizeException(&typ, &val, &tb);
	}

	else if (PyExceptionInstance_Check(typ)) {
		/* Raising an instance.  The value should be a dummy. */
		if (val && val != Py_None) {
			PyErr_SetString(PyExc_TypeError,
			  "instance exception may not have a separate value");
			goto failed_throw;
		}
		else {
			/* Normalize to raise <class>, <instance> */
			Py_XDECREF(val);
			val = typ;
			typ = PyExceptionInstance_Class(typ);
			Py_INCREF(typ);
		}
	}
	else {
		/* Not something you can raise.  throw() fails. */
		PyErr_Format(PyExc_TypeError,
			     "exceptions must be classes or instances "
			     "deriving from BaseException, not %s",
			     typ->ob_type->tp_name);
			goto failed_throw;
	}

	PyErr_Restore(typ, val, tb);
	return gen_send_ex(gen, Py_None, 1);

failed_throw:
	/* Didn't use our arguments, so restore their original refcounts */
	Py_DECREF(typ);
	Py_XDECREF(val);
	Py_XDECREF(tb);
	return NULL;
}


static PyObject *
gen_iternext(PyGenObject *gen)
{
	return gen_send_ex(gen, NULL, 0);
}


static PyMemberDef gen_memberlist[] = {
	{"gi_frame",	T_OBJECT, offsetof(PyGenObject, gi_frame),	READONLY},
	{"gi_running",	T_INT,    offsetof(PyGenObject, gi_running),	READONLY},
	{NULL}	/* Sentinel */
};

static PyMethodDef gen_methods[] = {
	{"send",(PyCFunction)gen_send, METH_O, send_doc},
	{"throw",(PyCFunction)gen_throw, METH_VARARGS, throw_doc},
	{"close",(PyCFunction)gen_close, METH_NOARGS, close_doc},
	{NULL, NULL}	/* Sentinel */
};

PyTypeObject PyGen_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"generator",				/* tp_name */
	sizeof(PyGenObject),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)gen_dealloc, 		/* tp_dealloc */
	0,					/* tp_print */
	0, 					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
 	0,					/* tp_doc */
 	(traverseproc)gen_traverse,		/* tp_traverse */
 	0,					/* tp_clear */
	0,					/* tp_richcompare */
	offsetof(PyGenObject, gi_weakreflist),	/* tp_weaklistoffset */
	PyObject_SelfIter,			/* tp_iter */
	(iternextfunc)gen_iternext,		/* tp_iternext */
	gen_methods,				/* tp_methods */
	gen_memberlist,				/* tp_members */
	0,					/* tp_getset */
	0,					/* tp_base */
	0,					/* tp_dict */

	0,					/* tp_descr_get */
	0,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	0,					/* tp_init */
	0,					/* tp_new */
	0,					/* tp_is_gc */
	0,					/* tp_bases */
	0,					/* tp_mro */
	0,					/* tp_cache */
	0,					/* tp_subclasses */
	0,					/* tp_weaklist */
};

PyObject *
PyGen_New(PyFrameObject *f)
{
	PyGenObject *gen = PyObject_NEW(PyGenObject, &PyGen_Type);
	if (gen == NULL) {
		Py_DECREF(f);
		return NULL;
	}
	gen->gi_frame = f;
	gen->gi_running = 0;
	gen->gi_weakreflist = NULL;
	return (PyObject *)gen;
}

int
PyGen_NeedsFinalizing(PyGenObject *gen)
{
	int i;
	PyFrameObject *f = gen->gi_frame;

	if (f == NULL || f->f_stacktop == NULL || f->f_iblock <= 0)
		return 0; /* no frame or empty blockstack == no finalization */

	/* Any block type besides a loop requires cleanup. */
	i = f->f_iblock;
	while (--i >= 0) {
		if (f->f_blockstack[i].b_type != SETUP_LOOP)
			return 1;
	}

	/* No blocks except loops, it's safe to skip finalization. */
	return 0;
}
