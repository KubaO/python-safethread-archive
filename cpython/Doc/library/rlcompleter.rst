
:mod:`rlcompleter` --- Completion function for GNU readline
===========================================================

.. module:: rlcompleter
   :synopsis: Python identifier completion, suitable for the GNU readline library.
.. sectionauthor:: Moshe Zadka <moshez@zadka.site.co.il>


The :mod:`rlcompleter` module defines a completion function suitable for the
:mod:`readline` module by completing valid Python identifiers and keywords.

When this module is imported on a Unix platform with the :mod:`readline` module
available, an instance of the :class:`Completer` class is automatically created
and its :meth:`complete` method is set as the :mod:`readline` completer.

Example::

   >>> import rlcompleter
   >>> import readline
   >>> readline.parse_and_bind("tab: complete")
   >>> readline. <TAB PRESSED>
   readline.__doc__          readline.get_line_buffer  readline.read_init_file
   readline.__file__         readline.insert_text      readline.set_completer
   readline.__name__         readline.parse_and_bind
   >>> readline.

The :mod:`rlcompleter` module is designed for use with Python's interactive
mode.  A user can add the following lines to his or her initialization file
(identified by the :envvar:`PYTHONSTARTUP` environment variable) to get
automatic :kbd:`Tab` completion::

   try:
       import readline
   except ImportError:
       print("Module readline not available.")
   else:
       import rlcompleter
       readline.parse_and_bind("tab: complete")

On platforms without :mod:`readline`, the :class:`Completer` class defined by
this module can still be used for custom purposes.


.. _completer-objects:

Completer Objects
-----------------

Completer objects have the following method:


.. method:: Completer.complete(text, state)

   Return the *state*th completion for *text*.

   If called for *text* that doesn't include a period character (``'.'``), it will
   complete from names currently defined in :mod:`__main__`, :mod:`builtins` and
   keywords (as defined by the :mod:`keyword` module).

   If called for a dotted name, it will try to evaluate anything without obvious
   side-effects (functions will not be evaluated, but it can generate calls to
   :meth:`__getattr__`) up to the last part, and find matches for the rest via the
   :func:`dir` function.

