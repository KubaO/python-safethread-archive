# WTF.  If my __future__ import is on the first line it gets ignored?!
from __future__ import shared_module

from threadtools import monitormethod, Monitor
from operator import isShareable
a = 42

def safesharedfunc():
    return a

def sharedfunc():
    global a
    a = 'moo'

def readloop():
    with open('/dev/zero', 'rb') as f:
        while f.read(1024):
            pass

class SharedClass:
    __shared__=True

    @staticmethod
    def sharedstaticmethod():
        return a

    @classmethod
    def sharedclassmethod(cls):
        return cls

    def normalmethod(self):
        pass

class MyMonitor(Monitor):
    __shared__ = True
    #__slots__ = ['hello', '__dict__']
    # XXX Hack!  tuple should be sometimes shareable
    __slots__ = frozendict.fromkeys(['hello', '__dict__'])

    def __init__(self):
        self.hello = 'world'

    @monitormethod
    def foo(self):
        return 'pink'

    def bar(self):
        return 'purple'

    baz = monitormethod(bar)

class Counter(Monitor):
    __shared__ = True

    def __init__(self):
        self.count = 0

    @monitormethod
    def tick(self):
        self.count += 1

    @monitormethod
    def value(self):
        return self.count
