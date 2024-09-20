#include "Python.h"


PyDoc_STRVAR(weakref_ref__doc__,
"ref(object) -- returns a weakref for 'object'.");

static PyObject *
weakref_ref(PyObject *self, PyObject *object)
{
    return PyWeakref_NewRef(object, NULL);
}

PyDoc_STRVAR(weakref_bind__doc__,
"bind(object, value) -- returns a weakbinding for 'object' and 'value'.");

static PyObject *
weakref_bind(PyObject *self, PyObject *args)
{
    PyObject *object, *value;

    if (!PyArg_ParseTuple(args, "OO:bind", &object, &value))
        return NULL;

    return PyWeakref_NewBinding(object, value);
}


static PyMethodDef
weakref_functions[] =  {
    {"ref",            weakref_ref,                    METH_O | METH_SHARED,
     weakref_ref__doc__},
    {"bind",           weakref_bind,                   METH_VARARGS | METH_SHARED,
     weakref_bind__doc__},
    {NULL, NULL, 0, NULL}
};


PyMODINIT_FUNC
init_weakref(void)
{
    PyObject *m;

    m = Py_InitModule5("_weakref", weakref_functions,
        "Weak-reference support module.", NULL, PYTHON_API_VERSION, 1);
    if (m != NULL) {
        Py_INCREF(&_PyWeakref_Type);
        PyModule_AddObject(m, "ReferenceType",
            (PyObject *)&_PyWeakref_Type);

        Py_INCREF(&_PyDeathQueue_Type);
        PyModule_AddObject(m, "DeathQueueType",
            (PyObject *)&_PyDeathQueue_Type);

        Py_INCREF(&_PyWeakBinding_Type);
        PyModule_AddObject(m, "WeakBindingType",
            (PyObject *)&_PyWeakBinding_Type);
    }
}
