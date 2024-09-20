# WTF.  If my __future__ import is on the first line it gets ignored?!
from __future__ import shared_module

from threadtools import monitormethod, Monitor, branch, condition, wait
from operator import isShareable
from time import sleep
a = 42

def safesharedfunc():
    return a

def sharedfunc():
    raise ValueError('moo')

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
    __slots__ = 'hello', '__dict__'

    def __init__(self):
        self.hello = 'world'

    @monitormethod
    def foo(self):
        return 'pink'

    def bar(self):
        return 'purple'

    @monitormethod
    def wait_test_child(self, cp_A, cp_B):
        def outside(cp_A, cp_B):
            cp_A.set()
            cp_B.wait()
        wait(self, outside, cp_A, cp_B)

    baz = monitormethod(bar)


class Counter(Monitor):
    __shared__ = True

    def __init__(self, highwatermark=1000):
        self.count = 0
        self.highwatermark = highwatermark

    @monitormethod
    def tick(self):
        self.count += 1

    @monitormethod
    def value(self):
        return self.count

    @condition
    def high(self):
        return self.count >= self.highwatermark

    @monitormethod
    def wait_for_high(self):
        wait(self.high)


class Checkpoint(Monitor):
    __shared__ = True

    def __init__(self):
        self.value = 0;

    @monitormethod
    def set(self):
        self.value = 1

    @condition
    def finished(self):
        return self.value

    @monitormethod
    def wait(self):
        wait(self.finished)


class Finalizable(Monitor):
    __shared__ = True
    __finalizeattrs__ = 'counter'

    def __init__(self, counter):
        self.counter = counter

    @monitormethod
    def __finalize__(self):
        super().__finalize__()
        self.counter.tick()


class Deadlocker(Monitor):
    __shared__ = True

    @monitormethod
    def chain(self, func, *args, **kwargs):
        func(*args, **kwargs)

    @monitormethod
    def pair_outer(self, m, cp_A, cp_B):
        cp_A.set()
        cp_B.wait()
        m.pair_inner()

    @monitormethod
    def pair_inner(self):
        readloop()

    @monitormethod
    def through_branch(self):
        with branch() as children:
            children.add(self.chain, sleep, 0)
