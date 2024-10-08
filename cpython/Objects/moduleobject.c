
/* Module object implementation */

#include "Python.h"
#include "structmember.h"

typedef struct {
	PyObject_HEAD
	PyObject *md_dict;
} PyModuleObject;

static PyMemberDef module_members[] = {
	{"__dict__", T_OBJECT, offsetof(PyModuleObject, md_dict), READONLY},
	{0}
};

PyObject *
PyModule_New(const char *name)
{
	return PyModule_NewEx(name, 0);
}

PyObject *
PyModule_NewEx(const char *name, int shared)
{
	PyModuleObject *m;
	PyObject *nameobj;
	m = PyObject_New(&PyModule_Type);
	if (m == NULL)
		return NULL;
	nameobj = PyUnicode_FromString(name);
	if (shared)
		m->md_dict = PyObject_CallObject((PyObject *)&PySharedDict_Type, NULL);
	else
		m->md_dict = PyDict_New();
	if (m->md_dict == NULL || nameobj == NULL)
		goto fail;
	if (PyDict_SetItemString(m->md_dict, "__name__", nameobj) != 0)
		goto fail;
	if (PyDict_SetItemString(m->md_dict, "__doc__", Py_None) != 0)
		goto fail;
	if (PyDict_SetItemString(m->md_dict, "__package__", Py_None) != 0)
		goto fail;
	Py_DECREF(nameobj);
	return (PyObject *)m;

 fail:
	Py_XDECREF(nameobj);
	Py_DECREF(m);
	return NULL;
}

PyObject *
PyModule_GetDict(PyObject *m)
{
	PyObject *d;
	if (!PyModule_Check(m)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	d = ((PyModuleObject *)m) -> md_dict;
	if (d == NULL)
		((PyModuleObject *)m) -> md_dict = d = PyDict_New();
	return d;
}

const char *
PyModule_GetName(PyObject *m)
{
	PyObject *d;
	PyObject *nameobj;
	const char *retval;
	if (!PyModule_Check(m)) {
		PyErr_BadArgument();
		return NULL;
	}
	d = ((PyModuleObject *)m)->md_dict;
	if (d == NULL || PyDict_GetItemStringEx(d, "__name__", &nameobj) > 0) {
		PyErr_SetString(PyExc_SystemError, "nameless module");
		return NULL;
	}
	if (nameobj == NULL)
		return NULL;
	retval = PyUnicode_AsString(nameobj);
	Py_DECREF(nameobj);
#warning XXX FIXME PyModule_GetName is racey (not thread-safe)
	return retval;
}

const char *
PyModule_GetFilename(PyObject *m)
{
	PyObject *d;
	PyObject *fileobj;
	if (!PyModule_Check(m)) {
		PyErr_BadArgument();
		return NULL;
	}
	d = ((PyModuleObject *)m)->md_dict;
	if (d == NULL ||
	    (fileobj = PyDict_GetItemString(d, "__file__")) == NULL ||
	    !PyUnicode_Check(fileobj))
	{
		PyErr_SetString(PyExc_SystemError, "module filename missing");
		return NULL;
	}
	return PyUnicode_AsString(fileobj);
}

void
_PyModule_Clear(PyObject *m)
{
	/* To make the execution order of destructors for global
	   objects a bit more predictable, we first zap all objects
	   whose name starts with a single underscore, before we clear
	   the entire dictionary.  We zap them by replacing them with
	   None, rather than deleting them from the dictionary, to
	   avoid rehashing the dictionary (to some extent). */

	Py_ssize_t pos;
	PyObject *key, *value;
	PyObject *d;

	d = ((PyModuleObject *)m)->md_dict;
	if (d == NULL)
		return;

	/* First, clear only names starting with a single underscore */
	pos = 0;
	while (PyDict_NextEx(d, &pos, &key, &value)) {
		if (value != Py_None && PyUnicode_Check(key)) {
			const char *s = PyUnicode_AsString(key);
			if (s[0] == '_' && s[1] != '_') {
				if (Py_VerboseFlag > 1)
				    PySys_WriteStderr("#   clear[1] %s\n", s);
				PyDict_SetItem(d, key, Py_None);
			}
		}
		Py_DECREF(key);
		Py_DECREF(value);
	}

	/* Next, clear all names except for __builtins__ */
	pos = 0;
	while (PyDict_NextEx(d, &pos, &key, &value)) {
		if (value != Py_None && PyUnicode_Check(key)) {
			const char *s = PyUnicode_AsString(key);
			if (s[0] != '_' || strcmp(s, "__builtins__") != 0) {
				if (Py_VerboseFlag > 1)
				    PySys_WriteStderr("#   clear[2] %s\n", s);
				PyDict_SetItem(d, key, Py_None);
			}
		}
		Py_DECREF(key);
		Py_DECREF(value);
	}

	/* Note: we leave __builtins__ in place, so that destructors
	   of non-global objects defined in this module can still use
	   builtins, in particularly 'None'. */

}

/* Methods */

static int
module_init(PyModuleObject *m, PyObject *args, PyObject *kwds)
{
	static char *kwlist[] = {"name", "doc", NULL};
	PyObject *dict, *name = Py_None, *doc = Py_None;
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "U|O:module.__init__",
                                         kwlist, &name, &doc))
		return -1;
	dict = m->md_dict;
	if (dict == NULL) {
		dict = PyDict_New();
		if (dict == NULL)
			return -1;
		m->md_dict = dict;
	}
	if (PyDict_SetItemString(dict, "__name__", name) < 0)
		return -1;
	if (PyDict_SetItemString(dict, "__doc__", doc) < 0)
		return -1;
	return 0;
}

static void
module_dealloc(PyModuleObject *m)
{
	if (m->md_dict != NULL) {
		// XXX FIXME Not safe during tp_dealloc, and unnecessary anyway?
		//_PyModule_Clear((PyObject *)m);
		Py_DECREF(m->md_dict);
	}
	PyObject_Del(m);
}

static PyObject *
module_repr(PyModuleObject *m)
{
	const char *name;
	const char *filename;

	name = PyModule_GetName((PyObject *)m);
	if (name == NULL) {
		PyErr_Clear();
		name = "?";
	}
	filename = PyModule_GetFilename((PyObject *)m);
	if (filename == NULL) {
		PyErr_Clear();
		return PyUnicode_FromFormat("<module '%s' (built-in)>", name);
	}
	return PyUnicode_FromFormat("<module '%s' from '%s'>", name, filename);
}

/* We only need a traverse function, no clear function: If the module
   is in a cycle, md_dict will be cleared as well, which will break
   the cycle. */
static int
module_traverse(PyModuleObject *m, visitproc visit, void *arg)
{
	Py_VISIT(m->md_dict);
	return 0;
}

static int
module_isshareable(PyModuleObject *m)
{
#warning XXX FIXME HACK pretending certain modules are shareable
	const char *name = PyModule_GetName((PyObject *)m);
	if (strcmp(name, "sys") == 0 || strcmp(name, "os") == 0 ||
			strcmp(name, "io") == 0)
		return 1;

	return PyObject_IsShareable(m->md_dict);
}

PyDoc_STRVAR(module_doc,
"module(name[, doc])\n\
\n\
Create a module object.\n\
The name must be a string; the optional doc argument can have any type.");

PyTypeObject PyModule_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"module",				/* tp_name */
	sizeof(PyModuleObject),			/* tp_size */
	0,					/* tp_itemsize */
	(destructor)module_dealloc,		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	(reprfunc)module_repr,			/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	PyObject_GenericSetAttr,		/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
		Py_TPFLAGS_BASETYPE |
		Py_TPFLAGS_SHAREABLE,		/* tp_flags */
	module_doc,				/* tp_doc */
	(traverseproc)module_traverse,		/* tp_traverse */
	0,					/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	0,					/* tp_iter */
	0,					/* tp_iternext */
	0,					/* tp_methods */
	module_members,				/* tp_members */
	0,					/* tp_getset */
	0,					/* tp_base */
	0,					/* tp_dict */
	0,					/* tp_descr_get */
	0,					/* tp_descr_set */
	offsetof(PyModuleObject, md_dict),	/* tp_dictoffset */
	(initproc)module_init,			/* tp_init */
	PyType_GenericNew,			/* tp_new */
	0,					/* tp_is_gc */
	0,					/* tp_bases */
	0,					/* tp_mro */
	0,					/* tp_cache */
	0,					/* tp_subclasses */
	0,					/* tp_weaklist */
	(isshareablefunc)module_isshareable,	/* tp_isshareable */
};
