import sys
import unittest
from time import sleep
from test import test_support

from test import sharedmodule
import threadtools

sharedmoduletest = """
>>> from test import sharedmodule
>>> import operator, threadtools
>>> from time import sleep

>>> operator.isShareable(sharedmodule)
True
>>> type(sharedmodule.__dict__)
<type 'shareddict'>
>>> sharedmodule.a
42

>>> sharedmodule.SharedClass.sharedstaticmethod()
42
>>> sharedmodule.SharedClass.sharedclassmethod()
<class 'test.sharedmodule.SharedClass'>

>>> sharedmodule.purple = []
Traceback (most recent call last):
  ...
TypeError: shareddict value must be shareable, 'list' object is not


>>> m = sharedmodule.MyMonitor()
>>> m.foo()
'pink'
>>> m.bar()
'purple'
>>> m.baz()
'purple'

>>> operator.isShareable(threadtools.Monitor)
True
>>> operator.isShareable(sharedmodule.MyMonitor)
True
>>> operator.isShareable(threadtools.Monitor())
True
>>> operator.isShareable(m)
True
>>> operator.isShareable(m.foo)
True
>>> operator.isShareable(m.bar)
True
>>> operator.isShareable(sharedmodule.MyMonitor.bar)
True
>>> operator.isShareable(sharedmodule.MyMonitor.__dict__['bar'])
True

>>> m.hello
Traceback (most recent call last):
  ...
AttributeError: hello
>>> m.enter(getattr, m, 'hello')
'world'
>>> m.plumage = 'green'
Traceback (most recent call last):
  ...
AttributeError: 'MyMonitor' object has no attribute 'plumage'
>>> m.enter(setattr, m, 'plumage', 'orange')
>>> m.plumage
Traceback (most recent call last):
  ...
AttributeError: 'MyMonitor' object has no attribute 'plumage'
>>> m.enter(getattr, m, 'plumage')
'orange'


>>> with threadtools.branch() as children:
...     children.add(sharedmodule.safesharedfunc)
>>> children.getresults()
[]

>>> with threadtools.branch() as children:
...     children.addresult(sharedmodule.safesharedfunc)
>>> children.getresults()
[42]
>>> children.getresults()
[]

>>> with threadtools.branch() as children:
...     children.add(sharedmodule.sharedfunc)
Traceback (most recent call last):
  ...
ValueError: moo
>>> children.getresults()
[]

>>> with threadtools.branch() as children:
...     children.addresult(sharedmodule.sharedfunc)
Traceback (most recent call last):
  ...
ValueError: moo
>>> children.getresults()
[]

>>> with threadtools.branch() as children:
...     1/0
Traceback (most recent call last):
  ...
ZeroDivisionError: int division or modulo by zero
>>> children.getresults()
[]


>>> with threadtools.branch() as children:
...     children.addresult(m.foo)
...     children.addresult(m.bar)
...     children.addresult(m.baz)
>>> children.getresults()
['pink', 'purple', 'purple']

>>> counter = sharedmodule.Counter()
>>> for i in range(10):
...     counter.tick()
>>> counter.value()
10
>>> with threadtools.branch() as children:
...     for i in range(15):
...         children.add(counter.tick)
>>> counter.value()
25

#>>> with threadtools.branch() as children:
#...     for i in range(10000):  # XXX arbitrary value.  Probably should be much higher
#...         children.add(sleep, 1)
#Traceback (most recent call last):
#  ...
#RuntimeError: branch.add can't spawn new thread

"""


class BranchTests(unittest.TestCase):
    def assertRaisesCause(self, excClass, excCause, func, *args, **kwargs):
        try:
            func(*args, **kwargs)
        except excClass as e:
            if not isinstance(excCause, tuple):
                excCause = (excCause,)
            cause = e.__cause__
            if not isinstance(cause, tuple):
                cause = (cause,)

            if len(excCause) != len(cause):
                raise
            for c, t in zip(cause, excCause):
                if not isinstance(c, t):
                    raise

            return

        excName = str(getattr(excClass, '__name__', excClass))
        objName = str(getattr(callableObj, '__name__', callableObj))
        raise self.failureException("%s(%s) not raised by %s" % (excName,
            excCause, objName))

    def test_main_and_child(self):
        def x():
            with threadtools.branch() as children:
                children.addresult(sharedmodule.sharedfunc)
                1/0
        self.assertRaisesCause(MultipleError, (ZeroDivisionError, ValueError), x)

    def test_multiple_child(self):
        def x():
            with threadtools.branch() as children:
                children.addresult(sharedmodule.sharedfunc)
                children.addresult(sharedmodule.sharedfunc)
                children.addresult(sharedmodule.sharedfunc)
        self.assertRaisesCause(MultipleError, (ValueError, ValueError, ValueError), x)

    def test_main_and_interrupted_child(self):
        def x():
            with threadtools.branch() as children:
                children.add(sharedmodule.readloop)
                1/0
        self.assertRaisesCause(ZeroDivisionError, (ZeroDivisionError, Interrupted), x)

    def test_nested_branch_failing_outer(self):
        def x():
            with threadtools.branch() as outer:
                with threadtools.branch() as inner:
                    inner.add(sharedmodule.readloop)
                    outer.add(sharedmodule.sharedfunc)
        self.assertRaisesCause(ValueError, (Interrupted, ValueError), x)

    def test_nested_branch_failing_outer_direct(self):
        def x():
            with threadtools.branch() as outer:
                with threadtools.branch() as inner:
                    outer.add(sharedmodule.sharedfunc)
                    sharedmodule.readloop()
        self.assertRaisesCause(ValueError, (Interrupted, ValueError), x)


class FinalizeTests(unittest.TestCase):
    def test_activates(self):
        counter = sharedmodule.Counter()
        obj = sharedmodule.Finalizable(counter)
        del obj

        # We want to wait long enough for this test to complete, but we
        # also don't want to expand every test run by 5 seconds.  A
        # polling sleep loop suffices.
        for i in range(50):
            if counter.value():
                break
            sleep(0.1)
        else:
            self.fail("__finalize__ never ran")

    def test_manual(self):
        # If __finalize__ gets called again by the finalizer thread it
        # will abort everything with an AttributeError
        def x():
            obj = sharedmodule.Finalizable(None)
            obj.__finalize__()
        self.assertRaises(AttributeError, x)


__test__ = {'sharedmoduletest' : sharedmoduletest}

def test_main(verbose=None):
    from test import test_sharedmodule
    test_support.run_doctest(test_sharedmodule, verbose)
    test_support.run_unittest(BranchTests, FinalizeTests)


if __name__ == "__main__":
    test_main(verbose=True)
