#!/usr/bin/env python

"""
Threaded wrapper for pystone
"""

from __future__ import shared_module

import sys
from threadtools import Monitor, monitormethod, collate
from test.pystone import LOOPS
from test.pystone import __file__ as pystone_filename

if pystone_filename.endswith('.pyc'):
    pystone_filename = pystone_filename[:-1]

__version__ = "0.1"


TRUE = 1

class Foo:
    __shared__ = True

class WrappedPystone(Monitor):
    __shared__ = True

    def __init__(self):
        #self.globals = {}
        #with open(pystone_filename, 'r') as f:
        #    exec(f.read(), self.globals)
        if 'test.pystone' in sys.modules:
            del sys.modules['test.pystone']
        import test.pystone
        self.globals = test.pystone.__dict__
        self.mod = test.pystone  # Needed to keep globals alive
        del sys.modules['test.pystone']
        pass

    @monitormethod
    def printfunc(self):
        print(self.globals['Proc0'])

    def x(self):
        pass

    @monitormethod
    def run(self, loops):
        self.globals['Proc0'](loops)
        #for i in range(loops):
            #Foo()
            #self.globals['Func3'](self.globals['Ident1'])
            #self.globals['Proc5']()
            #self.x()
            #x = []*5000
            #IntLoc1 = 2
            #IntLoc2 = 3
            #while IntLoc1 < IntLoc2:
            #    IntLoc3 = 5 * IntLoc1 - IntLoc2
            #    IntLoc3 = self.globals['Proc7'](IntLoc1, IntLoc2)
            #    IntLoc1 = IntLoc1 + 1
            #self.globals['Proc8'](self.globals['Array1Glob'], self.globals['Array2Glob'], IntLoc1, IntLoc3)

    def run_external(self, loops):
        #self.globals['Proc0'](loops)
        for i in range(loops):
            #self.globals['Func3'](self.globals['Ident1'])
            #self.globals['Proc5']()
            self.x()

def x(loops):
    n = 1.0
    for i in fakerange(loops):
        y()
        1.0+n
        pass

def y():
    pass


def main(threads, loops):
    # WEEEEE!  clock behaves differently for threads!
    from time import clock  # XXX workaround for clock not being shareable
    from time import time

    mods = [WrappedPystone() for i in range(threads)]
    #mods = list(range(threads))

    #starttime = clock()
    #for i in range(loops):
    #    pass
    #nulltime = clock() - starttime

    starttime = clock()
    starttime2 = time()

    with collate() as jobs:
        for mod in mods:
            jobs.add(mod.run, loops)
            #jobs.add(x, loops)

    #benchtime = clock() - starttime - nulltime
    benchtime = clock() - starttime
    benchtime2 = time() - starttime2
    if benchtime < 0.0001:
        benchtime = 0.0001
    stones = (threads * loops) / benchtime2

    #results = jobs.getresults()
    ## Crude hack!
    #results = [eval(result) for result in results]
    #results = [benchtime for benchtime, stones in results]
    #print(results)
    #print(benchtime)
    #print(stones)

    print("Thread-Pystone(%s) time for %d*%d passes = %g" % \
          (__version__, threads, loops, benchtime2))
    print("This machine benchmarks at %g pystones/second" % stones)
    #print(starttime, benchtime)
    #print(starttime2, benchtime2)


if __name__ == '__main__':
    import sys
    def error(msg):
        print(msg, end=' ', file=sys.stderr)
        print("usage: %s number_of_threads [number_of_loops]" % sys.argv[0], file=sys.stderr)
        sys.exit(100)
    nargs = len(sys.argv) - 1
    if nargs > 2:
        error("%d arguments are too many;" % nargs)
    elif nargs < 1:
        error("not enough arguments")

    try:
        threads = int(sys.argv[1])
    except ValueError:
        error("Invalid thread argument %r;" % sys.argv[1])

    if nargs >= 2:
        try:
            loops = int(sys.argv[1])
        except ValueError:
            error("Invalid argument %r;" % sys.argv[1])
    else:
        loops = LOOPS

    main(threads, loops)

