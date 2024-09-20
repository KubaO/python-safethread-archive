#include "Python.h"


PyDoc_STRVAR(weakref_ref__doc__,
"ref(object) -- returns a weakref for 'object'.");

static PyObject *
weakref_ref(PyObject *self, PyObject *object)
{
    return PyWeakref_NewRef(object, NULL);
}


static PyMethodDef
weakref_functions[] =  {
    {"ref",            weakref_ref,                    METH_O,
     weakref_ref__doc__},
    {NULL, NULL, 0, NULL}
};


PyMODINIT_FUNC
init_weakref(void)
{
    PyObject *m;

    m = Py_InitModule3("_weakref", weakref_functions,
                       "Weak-reference support module.");
    if (m != NULL) {
        Py_INCREF(&_PyWeakref_Type);
        PyModule_AddObject(m, "ReferenceType",
            (PyObject *)&_PyWeakref_Type);

        Py_INCREF(&_PyDeathQueue_Type);
        PyModule_AddObject(m, "DeathQueueType",
            (PyObject *)&_PyDeathQueue_Type);
    }
}
