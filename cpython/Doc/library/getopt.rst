
:mod:`getopt` --- Parser for command line options
=================================================

.. module:: getopt
   :synopsis: Portable parser for command line options; support both short and long option
              names.


This module helps scripts to parse the command line arguments in ``sys.argv``.
It supports the same conventions as the Unix :cfunc:`getopt` function (including
the special meanings of arguments of the form '``-``' and '``--``').  Long
options similar to those supported by GNU software may be used as well via an
optional third argument. This module provides two functions and an
exception:


.. function:: getopt(args, options[, long_options])

   Parses command line options and parameter list.  *args* is the argument list to
   be parsed, without the leading reference to the running program. Typically, this
   means ``sys.argv[1:]``. *options* is the string of option letters that the
   script wants to recognize, with options that require an argument followed by a
   colon (``':'``; i.e., the same format that Unix :cfunc:`getopt` uses).

   .. note::

      Unlike GNU :cfunc:`getopt`, after a non-option argument, all further arguments
      are considered also non-options. This is similar to the way non-GNU Unix systems
      work.

   *long_options*, if specified, must be a list of strings with the names of the
   long options which should be supported.  The leading ``'--'`` characters
   should not be included in the option name.  Long options which require an
   argument should be followed by an equal sign (``'='``).  To accept only long
   options, *options* should be an empty string.  Long options on the command line
   can be recognized so long as they provide a prefix of the option name that
   matches exactly one of the accepted options.  For example, if *long_options* is
   ``['foo', 'frob']``, the option :option:`--fo` will match as :option:`--foo`,
   but :option:`--f` will not match uniquely, so :exc:`GetoptError` will be raised.

   The return value consists of two elements: the first is a list of ``(option,
   value)`` pairs; the second is the list of program arguments left after the
   option list was stripped (this is a trailing slice of *args*).  Each
   option-and-value pair returned has the option as its first element, prefixed
   with a hyphen for short options (e.g., ``'-x'``) or two hyphens for long
   options (e.g., ``'--long-option'``), and the option argument as its
   second element, or an empty string if the option has no argument.  The
   options occur in the list in the same order in which they were found, thus
   allowing multiple occurrences.  Long and short options may be mixed.


.. function:: gnu_getopt(args, options[, long_options])

   This function works like :func:`getopt`, except that GNU style scanning mode is
   used by default. This means that option and non-option arguments may be
   intermixed. The :func:`getopt` function stops processing options as soon as a
   non-option argument is encountered.

   If the first character of the option string is '+', or if the environment
   variable POSIXLY_CORRECT is set, then option processing stops as soon as a
   non-option argument is encountered.


.. exception:: GetoptError

   This is raised when an unrecognized option is found in the argument list or when
   an option requiring an argument is given none. The argument to the exception is
   a string indicating the cause of the error.  For long options, an argument given
   to an option which does not require one will also cause this exception to be
   raised.  The attributes :attr:`msg` and :attr:`opt` give the error message and
   related option; if there is no specific option to which the exception relates,
   :attr:`opt` is an empty string.

.. XXX deprecated?
.. exception:: error

   Alias for :exc:`GetoptError`; for backward compatibility.

An example using only Unix style options:

   >>> import getopt
   >>> args = '-a -b -cfoo -d bar a1 a2'.split()
   >>> args
   ['-a', '-b', '-cfoo', '-d', 'bar', 'a1', 'a2']
   >>> optlist, args = getopt.getopt(args, 'abc:d:')
   >>> optlist
   [('-a', ''), ('-b', ''), ('-c', 'foo'), ('-d', 'bar')]
   >>> args
   ['a1', 'a2']

Using long option names is equally easy:

   >>> s = '--condition=foo --testing --output-file abc.def -x a1 a2'
   >>> args = s.split()
   >>> args
   ['--condition=foo', '--testing', '--output-file', 'abc.def', '-x', 'a1', 'a2']
   >>> optlist, args = getopt.getopt(args, 'x', [
   ...     'condition=', 'output-file=', 'testing'])
   >>> optlist
   [('--condition', 'foo'), ('--testing', ''), ('--output-file', 'abc.def'), ('-x', '')]
   >>> args
   ['a1', 'a2']

In a script, typical usage is something like this::

   import getopt, sys

   def main():
       try:
           opts, args = getopt.getopt(sys.argv[1:], "ho:v", ["help", "output="])
       except getopt.GetoptError as err:
           # print help information and exit:
           print(err) # will print something like "option -a not recognized"
           usage()
           sys.exit(2)
       output = None
       verbose = False
       for o, a in opts:
           if o == "-v":
               verbose = True
           elif o in ("-h", "--help"):
               usage()
               sys.exit()
           elif o in ("-o", "--output"):
               output = a
           else:
               assert False, "unhandled option"
       # ...

   if __name__ == "__main__":
       main()


.. seealso::

   Module :mod:`optparse`
      More object-oriented command line option parsing.

