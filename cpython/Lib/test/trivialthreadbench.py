#!/usr/bin/env python
from __future__ import shared_module
from threadtools import branch

def runthread(start, count):
    for i in range(start, start + count):
        pass
    return i

def main(split, total=10**7):
    if split:
        chunk = total // split
        print("Chunk of", chunk, "for", split, "threads.")
        with branch() as workers:
            for i in range(split):
                workers.addresult(runthread, i * chunk, chunk)
        print(workers.getresults())
    else:
        print("Running", total, "threadless.")
        runthread(0, total)

if __name__ == '__main__':
    raise RuntimeError("trivialthreadbench must not be the __main__ module")
