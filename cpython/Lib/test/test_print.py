"""Test correct operation of the print function.
"""

# In 2.6, this gives us the behavior we want.  In 3.0, it has
#  no function, but it still must parse correctly.
from __future__ import print_function

import unittest
from test import test_support

import sys
try:
    # 3.x
    from io import StringIO
except ImportError:
    # 2.x
    from StringIO import StringIO

NotDefined = object()

# A dispatch table all 8 combinations of providing
#  sep, end, and file
# I use this machinery so that I'm not just passing default
#  values to print, I'm eiher passing or not passing in the
#  arguments
dispatch = {
    (False, False, False):
     lambda args, sep, end, file: print(*args),
    (False, False, True):
     lambda args, sep, end, file: print(file=file, *args),
    (False, True,  False):
     lambda args, sep, end, file: print(end=end, *args),
    (False, True,  True):
     lambda args, sep, end, file: print(end=end, file=file, *args),
    (True,  False, False):
     lambda args, sep, end, file: print(sep=sep, *args),
    (True,  False, True):
     lambda args, sep, end, file: print(sep=sep, file=file, *args),
    (True,  True,  False):
     lambda args, sep, end, file: print(sep=sep, end=end, *args),
    (True,  True,  True):
     lambda args, sep, end, file: print(sep=sep, end=end, file=file, *args),
    }

# Class used to test __str__ and print
class ClassWith__str__:
    def __init__(self, x):
        self.x = x
    def __str__(self):
        return self.x

class TestPrint(unittest.TestCase):
    def check(self, expected, args,
            sep=NotDefined, end=NotDefined, file=NotDefined):
        # Capture sys.stdout in a StringIO.  Call print with args,
        #  and with sep, end, and file, if they're defined.  Result
        #  must match expected.

        # Look up the actual function to call, based on if sep, end, and file
        #  are defined
        fn = dispatch[(sep is not NotDefined,
                       end is not NotDefined,
                       file is not NotDefined)]

        with test_support.captured_stdout() as t:
            fn(args, sep, end, file)

        self.assertEqual(t.getvalue(), expected)

    def test_print(self):
        def x(expected, args, sep=NotDefined, end=NotDefined):
            # Run the test 2 ways: not using file, and using
            #  file directed to a StringIO

            self.check(expected, args, sep=sep, end=end)

            # When writing to a file, stdout is expected to be empty
            o = StringIO()
            self.check('', args, sep=sep, end=end, file=o)

            # And o will contain the expected output
            self.assertEqual(o.getvalue(), expected)

        x('\n', ())
        x('a\n', ('a',))
        x('None\n', (None,))
        x('1 2\n', (1, 2))
        x('1   2\n', (1, ' ', 2))
        x('1*2\n', (1, 2), sep='*')
        x('1 s', (1, 's'), end='')
        x('a\nb\n', ('a', 'b'), sep='\n')
        x('1.01', (1.0, 1), sep='', end='')
        x('1*a*1.3+', (1, 'a', 1.3), sep='*', end='+')
        x('a\n\nb\n', ('a\n', 'b'), sep='\n')
        x('\0+ +\0\n', ('\0', ' ', '\0'), sep='+')

        x('a\n b\n', ('a\n', 'b'))
        x('a\n b\n', ('a\n', 'b'), sep=None)
        x('a\n b\n', ('a\n', 'b'), end=None)
        x('a\n b\n', ('a\n', 'b'), sep=None, end=None)

        x('*\n', (ClassWith__str__('*'),))
        x('abc 1\n', (ClassWith__str__('abc'), 1))

#        # 2.x unicode tests
#        x(u'1 2\n', ('1', u'2'))
#        x(u'u\1234\n', (u'u\1234',))
#        x(u'  abc 1\n', (' ', ClassWith__str__(u'abc'), 1))

        # errors
        self.assertRaises(TypeError, print, '', sep=3)
        self.assertRaises(TypeError, print, '', end=3)
        self.assertRaises(AttributeError, print, '', file='')

def test_main():
    test_support.run_unittest(TestPrint)

if __name__ == "__main__":
    test_main()
