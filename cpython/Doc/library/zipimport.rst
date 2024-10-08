
:mod:`zipimport` --- Import modules from Zip archives
=====================================================

.. module:: zipimport
   :synopsis: support for importing Python modules from ZIP archives.
.. moduleauthor:: Just van Rossum <just@letterror.com>


This module adds the ability to import Python modules (:file:`\*.py`,
:file:`\*.py[co]`) and packages from ZIP-format archives. It is usually not
needed to use the :mod:`zipimport` module explicitly; it is automatically used
by the builtin :keyword:`import` mechanism for ``sys.path`` items that are paths
to ZIP archives.

Typically, ``sys.path`` is a list of directory names as strings.  This module
also allows an item of ``sys.path`` to be a string naming a ZIP file archive.
The ZIP archive can contain a subdirectory structure to support package imports,
and a path within the archive can be specified to only import from a
subdirectory.  For example, the path :file:`/tmp/example.zip/lib/` would only
import from the :file:`lib/` subdirectory within the archive.

Any files may be present in the ZIP archive, but only files :file:`.py` and
:file:`.py[co]` are available for import.  ZIP import of dynamic modules
(:file:`.pyd`, :file:`.so`) is disallowed. Note that if an archive only contains
:file:`.py` files, Python will not attempt to modify the archive by adding the
corresponding :file:`.pyc` or :file:`.pyo` file, meaning that if a ZIP archive
doesn't contain :file:`.pyc` files, importing may be rather slow.

.. seealso::

   `PKZIP Application Note <http://www.pkware.com/documents/casestudies/APPNOTE.TXT>`_
      Documentation on the ZIP file format by Phil Katz, the creator of the format and
      algorithms used.

   :pep:`0273` - Import Modules from Zip Archives
      Written by James C. Ahlstrom, who also provided an implementation. Python 2.3
      follows the specification in PEP 273, but uses an implementation written by Just
      van Rossum that uses the import hooks described in PEP 302.

   :pep:`0302` - New Import Hooks
      The PEP to add the import hooks that help this module work.


This module defines an exception:

.. exception:: ZipImportError

   Exception raised by zipimporter objects. It's a subclass of :exc:`ImportError`,
   so it can be caught as :exc:`ImportError`, too.


.. _zipimporter-objects:

zipimporter Objects
-------------------

:class:`zipimporter` is the class for importing ZIP files.

.. class:: zipimporter(archivepath)

   Create a new zipimporter instance. *archivepath* must be a path to a ZIP file.
   :exc:`ZipImportError` is raised if *archivepath* doesn't point to a valid ZIP
   archive.

   *archivepath* can also contain a path within the ZIP file -- the importer
   object will then look under that path instead of the ZIP file root.  For
   example, an *archivepath* of :file:`foo/bar.zip/lib` will look for modules
   in the :file:`lib` directory inside the ZIP file :file:`foo/bar.zip`
   (provided that it exists).


.. method:: zipimporter.find_module(fullname[, path])

   Search for a module specified by *fullname*. *fullname* must be the fully
   qualified (dotted) module name. It returns the zipimporter instance itself if
   the module was found, or :const:`None` if it wasn't. The optional *path*
   argument is ignored---it's there for  compatibility with the importer protocol.


.. method:: zipimporter.get_code(fullname)

   Return the code object for the specified module. Raise :exc:`ZipImportError` if
   the module couldn't be found.


.. method:: zipimporter.get_data(pathname)

   Return the data associated with *pathname*. Raise :exc:`IOError` if the file
   wasn't found.


.. method:: zipimporter.get_source(fullname)

   Return the source code for the specified module. Raise :exc:`ZipImportError` if
   the module couldn't be found, return :const:`None` if the archive does contain
   the module, but has no source for it.


.. method:: zipimporter.is_package(fullname)

   Return True if the module specified by *fullname* is a package. Raise
   :exc:`ZipImportError` if the module couldn't be found.


.. method:: zipimporter.load_module(fullname)

   Load the module specified by *fullname*. *fullname* must be the fully qualified
   (dotted) module name. It returns the imported module, or raises
   :exc:`ZipImportError` if it wasn't found.


.. attribute:: zipimporter.archive

   The file name of the importer's associated ZIP file.


.. attribute:: zipimporter.prefix

   The path within the ZIP file where modules are searched; see
   :class:`zipimporter` for details.


.. _zipimport-examples:

Examples
--------

Here is an example that imports a module from a ZIP archive - note that the
:mod:`zipimport` module is not explicitly used. ::

   $ unzip -l /tmp/example.zip
   Archive:  /tmp/example.zip
     Length     Date   Time    Name
    --------    ----   ----    ----
        8467  11-26-02 22:30   jwzthreading.py
    --------                   -------
        8467                   1 file
   $ ./python
   Python 2.3 (#1, Aug 1 2003, 19:54:32) 
   >>> import sys
   >>> sys.path.insert(0, '/tmp/example.zip')  # Add .zip file to front of path
   >>> import jwzthreading
   >>> jwzthreading.__file__
   '/tmp/example.zip/jwzthreading.py'

