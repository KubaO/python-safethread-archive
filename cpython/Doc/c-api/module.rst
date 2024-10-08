.. highlightlang:: c

.. _moduleobjects:

Module Objects
--------------

.. index:: object: module

There are only a few functions special to module objects.


.. cvar:: PyTypeObject PyModule_Type

   .. index:: single: ModuleType (in module types)

   This instance of :ctype:`PyTypeObject` represents the Python module type.  This
   is exposed to Python programs as ``types.ModuleType``.


.. cfunction:: int PyModule_Check(PyObject *p)

   Return true if *p* is a module object, or a subtype of a module object.


.. cfunction:: int PyModule_CheckExact(PyObject *p)

   Return true if *p* is a module object, but not a subtype of
   :cdata:`PyModule_Type`.


.. cfunction:: PyObject* PyModule_New(const char *name)

   .. index::
      single: __name__ (module attribute)
      single: __doc__ (module attribute)
      single: __file__ (module attribute)

   Return a new module object with the :attr:`__name__` attribute set to *name*.
   Only the module's :attr:`__doc__` and :attr:`__name__` attributes are filled in;
   the caller is responsible for providing a :attr:`__file__` attribute.


.. cfunction:: PyObject* PyModule_GetDict(PyObject *module)

   .. index:: single: __dict__ (module attribute)

   Return the dictionary object that implements *module*'s namespace; this object
   is the same as the :attr:`__dict__` attribute of the module object.  This
   function never fails.  It is recommended extensions use other
   :cfunc:`PyModule_\*` and :cfunc:`PyObject_\*` functions rather than directly
   manipulate a module's :attr:`__dict__`.


.. cfunction:: char* PyModule_GetName(PyObject *module)

   .. index::
      single: __name__ (module attribute)
      single: SystemError (built-in exception)

   Return *module*'s :attr:`__name__` value.  If the module does not provide one,
   or if it is not a string, :exc:`SystemError` is raised and *NULL* is returned.


.. cfunction:: char* PyModule_GetFilename(PyObject *module)

   .. index::
      single: __file__ (module attribute)
      single: SystemError (built-in exception)

   Return the name of the file from which *module* was loaded using *module*'s
   :attr:`__file__` attribute.  If this is not defined, or if it is not a string,
   raise :exc:`SystemError` and return *NULL*.


.. cfunction:: int PyModule_AddObject(PyObject *module, const char *name, PyObject *value)

   Add an object to *module* as *name*.  This is a convenience function which can
   be used from the module's initialization function.  This steals a reference to
   *value*.  Return ``-1`` on error, ``0`` on success.


.. cfunction:: int PyModule_AddIntConstant(PyObject *module, const char *name, long value)

   Add an integer constant to *module* as *name*.  This convenience function can be
   used from the module's initialization function. Return ``-1`` on error, ``0`` on
   success.


.. cfunction:: int PyModule_AddStringConstant(PyObject *module, const char *name, const char *value)

   Add a string constant to *module* as *name*.  This convenience function can be
   used from the module's initialization function.  The string *value* must be
   null-terminated.  Return ``-1`` on error, ``0`` on success.


.. cfunction:: int PyModule_AddIntMacro(PyObject *module, macro)

   Add an int constant to *module*. The name and the value are taken from 
   *macro*. For example ``PyModule_AddConstant(module, AF_INET)`` adds the int
   constant *AF_INET* with the value of *AF_INET* to *module*.
   Return ``-1`` on error, ``0`` on success.


.. cfunction:: int PyModule_AddStringMacro(PyObject *module, macro)

   Add a string constant to *module*.

