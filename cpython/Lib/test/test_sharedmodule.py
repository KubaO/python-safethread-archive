import sys

from test import test_support

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
>>> sharedmodule.b = 7
Traceback (most recent call last):
  ...
TypeError: shareddict instance cannot be modified
>>> sharedmodule.a = 7
Traceback (most recent call last):
  ...
TypeError: shareddict instance cannot be modified
>>> sharedmodule.sharedfunc()
Traceback (most recent call last):
  ...
TypeError: shareddict instance cannot be modified

>>> sharedmodule.SharedClass.sharedstaticmethod()
42
>>> sharedmodule.SharedClass.sharedclassmethod()
<class 'test.sharedmodule.SharedClass'>


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


>>> with threadtools.collate() as c:
...     c.add(sharedmodule.safesharedfunc)
>>> c.getresults()
[]

>>> with threadtools.collate() as c:
...     c.addresult(sharedmodule.safesharedfunc)
>>> c.getresults()
[42]
>>> c.getresults()
[]

>>> with threadtools.collate() as c:
...     c.add(sharedmodule.sharedfunc)
Traceback (most recent call last):
  ...
TypeError: shareddict instance cannot be modified
>>> c.getresults()
[]

>>> with threadtools.collate() as c:
...     c.addresult(sharedmodule.sharedfunc)
Traceback (most recent call last):
  ...
TypeError: shareddict instance cannot be modified
>>> c.getresults()
[]

>>> with threadtools.collate() as c:
...     1/0
Traceback (most recent call last):
  ...
ZeroDivisionError: int division or modulo by zero
>>> c.getresults()
[]

# Alas, the ... in this exception line could allow entire extra
# exceptions to be swallowed
>>> with threadtools.collate() as c:  # doctest: +ELLIPSIS
...     c.addresult(sharedmodule.sharedfunc)
...     1/0
Traceback (most recent call last):
  ...
MultipleError: [(<type 'ZeroDivisionError'>, 'int division or modulo by zero', ...), (<type 'TypeError'>, 'shareddict instance cannot be modified', ...)]

>>> with threadtools.collate() as c:  # doctest: +ELLIPSIS
...     c.addresult(sharedmodule.sharedfunc)
...     c.addresult(sharedmodule.sharedfunc)
...     c.addresult(sharedmodule.sharedfunc)
Traceback (most recent call last):
  ...
MultipleError: [(<type 'TypeError'>, 'shareddict instance cannot be modified', ...), (<type 'TypeError'>, 'shareddict instance cannot be modified', ...), (<type 'TypeError'>, 'shareddict instance cannot be modified', ...)]


>>> with threadtools.collate() as c:
...     c.addresult(m.foo)
...     c.addresult(m.bar)
...     c.addresult(m.baz)
>>> c.getresults()
['pink', 'purple', 'purple']

>>> counter = sharedmodule.Counter()
>>> for i in range(10):
...     counter.tick()
>>> counter.value()
10
>>> with threadtools.collate() as c:
...     for i in range(15):
...         c.add(counter.tick)
>>> counter.value()
25

>>> with threadtools.collate() as c:
...     for i in range(10000):  # XXX arbitrary value.  Probably should be much higher
...         c.add(sleep, 1)
Traceback (most recent call last):
  ...
RuntimeError: collate.add can't spawn new thread


# This isn't the intended exception, but it is what the code as-written
# should produce.  This test can be changed once I come up with a better
# way of storing Interrupted on another exception.
>>> with threadtools.collate() as c:  # doctest: +ELLIPSIS
...     c.add(sharedmodule.readloop)
...     1/0
Traceback (most recent call last):
  ...
MultipleError: [(<type 'ZeroDivisionError'>, 'int division or modulo by zero', ...), (<type 'Interrupted'>, 'I/O operation interrupted by parent', ...)]

"""

__test__ = {'sharedmoduletest' : sharedmoduletest}

def test_main():
    test_support.run_doctest(sys.modules[__name__])


if __name__ == "__main__":
    test_main()
