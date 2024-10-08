.. XXX this whole file is outdated

:mod:`StringIO` --- Read and write strings as files
===================================================

.. module:: StringIO
   :synopsis: Read and write strings as if they were files.


This module implements a file-like class, :class:`StringIO`, that reads and
writes a string buffer (also known as *memory files*).  See the description of
file objects for operations (section :ref:`bltin-file-objects`). (For
standard strings, see :class:`str`.)


.. class:: StringIO([buffer])

   When a :class:`StringIO` object is created, it can be initialized to an existing
   string by passing the string to the constructor. If no string is given, the
   :class:`StringIO` will start empty. In both cases, the initial file position
   starts at zero.

The following methods of :class:`StringIO` objects require special mention:


.. method:: StringIO.getvalue()

   Retrieve the entire contents of the "file" at any time before the
   :class:`StringIO` object's :meth:`close` method is called.


.. method:: StringIO.close()

   Free the memory buffer.

Example usage::

   import StringIO

   output = StringIO.StringIO()
   output.write('First line.\n')
   print('Second line.', file=output)

   # Retrieve file contents -- this will be
   # 'First line.\nSecond line.\n'
   contents = output.getvalue()

   # Close object and discard memory buffer -- 
   # .getvalue() will now raise an exception.
   output.close()


:mod:`cStringIO` --- Faster version of :mod:`StringIO`
======================================================

.. module:: cStringIO
   :synopsis: Faster version of StringIO, but not subclassable.
.. moduleauthor:: Jim Fulton <jim@zope.com>
.. sectionauthor:: Fred L. Drake, Jr. <fdrake@acm.org>


The module :mod:`cStringIO` provides an interface similar to that of the
:mod:`StringIO` module.  Heavy use of :class:`StringIO.StringIO` objects can be
made more efficient by using the function :func:`StringIO` from this module
instead.

Since this module provides a factory function which returns objects of built-in
types, there's no way to build your own version using subclassing.  Use the
original :mod:`StringIO` module in that case.

Unlike the memory files implemented by the :mod:`StringIO` module, those
provided by this module are not able to accept strings that cannot be
encoded in plain ASCII.

Calling :func:`StringIO` with a string parameter populates
the object with the buffer representation of the string, instead of
encoding the string. 

Another difference from the :mod:`StringIO` module is that calling
:func:`StringIO` with a string parameter creates a read-only object. Unlike an
object created without a string parameter, it does not have write methods.
These objects are not generally visible.  They turn up in tracebacks as
:class:`StringI` and :class:`StringO`.

The following data objects are provided as well:


.. data:: InputType

   The type object of the objects created by calling :func:`StringIO` with a string
   parameter.


.. data:: OutputType

   The type object of the objects returned by calling :func:`StringIO` with no
   parameters.

There is a C API to the module as well; refer to the module source for  more
information.

Example usage::

   import cStringIO

   output = cStringIO.StringIO()
   output.write('First line.\n')
   print('Second line.', file=output)

   # Retrieve file contents -- this will be
   # 'First line.\nSecond line.\n'
   contents = output.getvalue()

   # Close object and discard memory buffer -- 
   # .getvalue() will now raise an exception.
   output.close()

