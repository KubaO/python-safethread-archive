
/* Use this file as a template to start implementing a module that
   also declares object types. All occurrences of 'Xxo' should be changed
   to something reasonable for your objects. After that, all other
   occurrences of 'xx' should be changed to something reasonable for your
   module. If your module is named foo your sourcefile should be named
   foomodule.c.

   You will probably want to delete all references to 'x_attr' and add
   your own types of attributes instead.  Maybe you want to name your
   local variables other than 'self'.  If your object type is needed in
   other files, you'll have to create a file "foobarobject.h"; see
   intobject.h for an example. */

/* Xxo objects */

#include "Python.h"

static PyObject *ErrorObject;

typedef struct {
	PyObject_HEAD
	PyObject	*x_attr;	/* Attributes dictionary */
} XxoObject;

static PyTypeObject Xxo_Type;

#define XxoObject_Check(v)	(Py_TYPE(v) == &Xxo_Type)

static XxoObject *
newXxoObject(PyObject *arg)
{
	XxoObject *self;
	self = PyObject_New(XxoObject, &Xxo_Type);
	if (self == NULL)
		return NULL;
	self->x_attr = NULL;
	return self;
}

/* Xxo methods */

static void
Xxo_dealloc(XxoObject *self)
{
	Py_XDECREF(self->x_attr);
	PyObject_Del(self);
}

static PyObject *
Xxo_demo(XxoObject *self, PyObject *args)
{
	if (!PyArg_ParseTuple(args, ":demo"))
		return NULL;
	Py_INCREF(Py_None);
	return Py_None;
}

static PyMethodDef Xxo_methods[] = {
	{"demo",	(PyCFunction)Xxo_demo,	METH_VARARGS,
		PyDoc_STR("demo() -> None")},
	{NULL,		NULL}		/* sentinel */
};

static PyObject *
Xxo_getattr(XxoObject *self, char *name)
{
	if (self->x_attr != NULL) {
		PyObject *v = PyDict_GetItemString(self->x_attr, name);
		if (v != NULL) {
			Py_INCREF(v);
			return v;
		}
	}
	return Py_FindMethod(Xxo_methods, (PyObject *)self, name);
}

static int
Xxo_setattr(XxoObject *self, char *name, PyObject *v)
{
	if (self->x_attr == NULL) {
		self->x_attr = PyDict_New();
		if (self->x_attr == NULL)
			return -1;
	}
	if (v == NULL) {
		int rv = PyDict_DelItemString(self->x_attr, name);
		if (rv < 0)
			PyErr_SetString(PyExc_AttributeError,
			        "delete non-existing Xxo attribute");
		return rv;
	}
	else
		return PyDict_SetItemString(self->x_attr, name, v);
}

static PyTypeObject Xxo_Type = {
	/* The ob_type field must be initialized in the module init function
	 * to be portable to Windows without using C++. */
	PyVarObject_HEAD_INIT(NULL, 0)
	"xxmodule.Xxo",		/*tp_name*/
	sizeof(XxoObject),	/*tp_basicsize*/
	0,			/*tp_itemsize*/
	/* methods */
	(destructor)Xxo_dealloc, /*tp_dealloc*/
	0,			/*tp_print*/
	(getattrfunc)Xxo_getattr, /*tp_getattr*/
	(setattrfunc)Xxo_setattr, /*tp_setattr*/
	0,			/*tp_compare*/
	0,			/*tp_repr*/
	0,			/*tp_as_number*/
	0,			/*tp_as_sequence*/
	0,			/*tp_as_mapping*/
	0,			/*tp_hash*/
        0,                      /*tp_call*/
        0,                      /*tp_str*/
        0,                      /*tp_getattro*/
        0,                      /*tp_setattro*/
        0,                      /*tp_as_buffer*/
        Py_TPFLAGS_DEFAULT,     /*tp_flags*/
        0,                      /*tp_doc*/
        0,                      /*tp_traverse*/
        0,                      /*tp_clear*/
        0,                      /*tp_richcompare*/
        0,                      /*tp_weaklistoffset*/
        0,                      /*tp_iter*/
        0,                      /*tp_iternext*/
        0,                      /*tp_methods*/
        0,                      /*tp_members*/
        0,                      /*tp_getset*/
        0,                      /*tp_base*/
        0,                      /*tp_dict*/
        0,                      /*tp_descr_get*/
        0,                      /*tp_descr_set*/
        0,                      /*tp_dictoffset*/
        0,                      /*tp_init*/
        0,                      /*tp_new*/
        0,                      /*tp_is_gc*/
};
/* --------------------------------------------------------------------- */

/* Function of two integers returning integer */

PyDoc_STRVAR(xx_foo_doc,
"foo(i,j)\n\
\n\
Return the sum of i and j.");

static PyObject *
xx_foo(PyObject *self, PyObject *args)
{
	long i, j;
	long res;
	if (!PyArg_ParseTuple(args, "ll:foo", &i, &j))
		return NULL;
	res = i+j; /* XXX Do something here */
	return PyLong_FromLong(res);
}


/* Function of no arguments returning new Xxo object */

static PyObject *
xx_new(PyObject *self, PyObject *args)
{
	XxoObject *rv;

	if (!PyArg_ParseTuple(args, ":new"))
		return NULL;
	rv = newXxoObject(args);
	if (rv == NULL)
		return NULL;
	return (PyObject *)rv;
}

/* Example with subtle bug from extensions manual ("Thin Ice"). */

static PyObject *
xx_bug(PyObject *self, PyObject *args)
{
	PyObject *list, *item;

	if (!PyArg_ParseTuple(args, "O:bug", &list))
		return NULL;

	item = PyList_GetItem(list, 0);
	/* Py_INCREF(item); */
	PyList_SetItem(list, 1, PyLong_FromLong(0L));
	PyObject_Print(item, stdout, 0);
	printf("\n");
	/* Py_DECREF(item); */

	Py_INCREF(Py_None);
	return Py_None;
}

/* Test bad format character */

static PyObject *
xx_roj(PyObject *self, PyObject *args)
{
	PyObject *a;
	long b;
	if (!PyArg_ParseTuple(args, "O#:roj", &a, &b))
		return NULL;
	Py_INCREF(Py_None);
	return Py_None;
}


/* ---------- */

static PyTypeObject Str_Type = {
	/* The ob_type field must be initialized in the module init function
	 * to be portable to Windows without using C++. */
	PyVarObject_HEAD_INIT(NULL, 0)
	"xxmodule.Str",		/*tp_name*/
	0,			/*tp_basicsize*/
	0,			/*tp_itemsize*/
	/* methods */
	0,			/*tp_dealloc*/
	0,			/*tp_print*/
	0,			/*tp_getattr*/
	0,			/*tp_setattr*/
	0,			/*tp_compare*/
	0,			/*tp_repr*/
	0,			/*tp_as_number*/
	0,			/*tp_as_sequence*/
	0,			/*tp_as_mapping*/
	0,			/*tp_hash*/
	0,			/*tp_call*/
	0,			/*tp_str*/
	0,			/*tp_getattro*/
	0,			/*tp_setattro*/
	0,			/*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
	0,			/*tp_doc*/
	0,			/*tp_traverse*/
	0,			/*tp_clear*/
	0,			/*tp_richcompare*/
	0,			/*tp_weaklistoffset*/
	0,			/*tp_iter*/
	0,			/*tp_iternext*/
	0,			/*tp_methods*/
	0,			/*tp_members*/
	0,			/*tp_getset*/
	0, /* see initxx */	/*tp_base*/
	0,			/*tp_dict*/
	0,			/*tp_descr_get*/
	0,			/*tp_descr_set*/
	0,			/*tp_dictoffset*/
	0,			/*tp_init*/
	0,			/*tp_new*/
	0,			/*tp_is_gc*/
};

/* ---------- */

static PyObject *
null_richcompare(PyObject *self, PyObject *other, int op)
{
	Py_INCREF(Py_NotImplemented);
	return Py_NotImplemented;
}

static PyTypeObject Null_Type = {
	/* The ob_type field must be initialized in the module init function
	 * to be portable to Windows without using C++. */
	PyVarObject_HEAD_INIT(NULL, 0)
	"xxmodule.Null",	/*tp_name*/
	0,			/*tp_basicsize*/
	0,			/*tp_itemsize*/
	/* methods */
	0,			/*tp_dealloc*/
	0,			/*tp_print*/
	0,			/*tp_getattr*/
	0,			/*tp_setattr*/
	0,			/*tp_compare*/
	0,			/*tp_repr*/
	0,			/*tp_as_number*/
	0,			/*tp_as_sequence*/
	0,			/*tp_as_mapping*/
	0,			/*tp_hash*/
	0,			/*tp_call*/
	0,			/*tp_str*/
	0,			/*tp_getattro*/
	0,			/*tp_setattro*/
	0,			/*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
	0,			/*tp_doc*/
	0,			/*tp_traverse*/
	0,			/*tp_clear*/
	null_richcompare,	/*tp_richcompare*/
	0,			/*tp_weaklistoffset*/
	0,			/*tp_iter*/
	0,			/*tp_iternext*/
	0,			/*tp_methods*/
	0,			/*tp_members*/
	0,			/*tp_getset*/
	0, /* see initxx */	/*tp_base*/
	0,			/*tp_dict*/
	0,			/*tp_descr_get*/
	0,			/*tp_descr_set*/
	0,			/*tp_dictoffset*/
	0,			/*tp_init*/
	0, /* see initxx */	/*tp_new*/
	0,			/*tp_is_gc*/
};


/* ---------- */


/* List of functions defined in the module */

static PyMethodDef xx_methods[] = {
	{"roj",		xx_roj,		METH_VARARGS,
		PyDoc_STR("roj(a,b) -> None")},
	{"foo",		xx_foo,		METH_VARARGS,
	 	xx_foo_doc},
	{"new",		xx_new,		METH_VARARGS,
		PyDoc_STR("new() -> new Xx object")},
	{"bug",		xx_bug,		METH_VARARGS,
		PyDoc_STR("bug(o) -> None")},
	{NULL,		NULL}		/* sentinel */
};

PyDoc_STRVAR(module_doc,
"This is a template module just for instruction.");

/* Initialization function for the module (*must* be called initxx) */

PyMODINIT_FUNC
initxx(void)
{
	PyObject *m;

	/* Due to cross platform compiler issues the slots must be filled
	 * here. It's required for portability to Windows without requiring
	 * C++. */
	Null_Type.tp_base = &PyBaseObject_Type;
	Null_Type.tp_new = PyType_GenericNew;
	Str_Type.tp_base = &PyUnicode_Type;

	/* Finalize the type object including setting type of the new type
	 * object; doing it here is required for portability, too. */
	if (PyType_Ready(&Xxo_Type) < 0)
		return;

	/* Create the module and add the functions */
	m = Py_InitModule3("xx", xx_methods, module_doc);
	if (m == NULL)
		return;

	/* Add some symbolic constants to the module */
	if (ErrorObject == NULL) {
		ErrorObject = PyErr_NewException("xx.error", NULL, NULL);
		if (ErrorObject == NULL)
			return;
	}
	Py_INCREF(ErrorObject);
	PyModule_AddObject(m, "error", ErrorObject);

	/* Add Str */
	if (PyType_Ready(&Str_Type) < 0)
		return;
	PyModule_AddObject(m, "Str", (PyObject *)&Str_Type);

	/* Add Null */
	if (PyType_Ready(&Null_Type) < 0)
		return;
	PyModule_AddObject(m, "Null", (PyObject *)&Null_Type);
}
