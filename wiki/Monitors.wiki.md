# User-defined shareable, mutable objects

Given how limiting it is to be unable to [share mutable objects](Shareability.wiki.md), a way is needed to get around the restrictions.  Monitors are the primary tool for this.

Essentially what a Monitor does is it creates a wall around your object — nothing unshareable gets in or out.  It then lets one thread at a time inside this wall, forcing any other threads to wait until the first one leaves.

```python
# counter.py
from __future__ import shared_module

from threadtools import Monitor, monitormethod

class Counter(Monitor):
    """A simple counter, shared between threads"""
    __shared__ = True  # More shared_module boilerplate

    def __init__(self):
        self.count = 0
    
    @monitormethod
    def tick(self):
        self.count += 1
    
    @monitormethod
    def value(self):
        return self.count
```

The `count` attribute is hidden within the `Counter` instances.  `@monitormethod` indicates that a function should run within the Monitor.

`Counter` can be used like this:

```python
# main.py
from __future__ import shared_module

from counter import Counter
from threadtools import branch

def work(c):
    for i in range(20):
        c.tick()

def main():
    c = Counter()

    with branch() as children:
        for i in range(10):
            children.add(work, c)
    
    print("Number of ticks:", c.value())
```

When `main` is called it creates a `Counter`, then spawns 10 threads to access it.  Each of those threads calls `c.tick()` 20 times.  After the threads exit the total number of ticks is printed: 200.

## Caveat
Due to some problems with how python runs its startup script, the `__future__` import does not work.  To work around this you should explicitly import `main.py`, then run its `main` function.

```
$ ./python -c 'import main; main.main()'
Number of ticks: 200
$ 
```

## Conditions
In addition to allowing you to share objects between threads, Monitors also provide a facility for waiting until you can do a certain activity on those those objects - called a condition.

```python
# queue.py
from __future__ import shared_module

from collections import deque
from threadtools import Monitor, monitormethod, condition, wait

class Queue(Monitor):
    """A simple thread-safe queue"""
    __shared__ = True  # More shared_module boilerplate

    def __init__(self, limit=None):
        self.data = deque()
        self.limit = limit
    
    @condition
    def _notfull(self):
        if self.limit is None:
            return True
        return len(self.data) < self.limit
    
    @condition
    def _notempty(self):
        return bool(self.data)
    
    @monitormethod
    def put(self, value):
        wait(self._notfull)
        self.data.append(value)
    
    @monitormethod
    def get(self):
        wait(self._notempty)
        return self.data.popleft()
```

Although similar to traditional conditions used for threading, these are defined as part of the class, which gives them some interesting properties:

* You cannot have unique wakeup predicates for each thread in a Monitor - but you could give them each their own Monitor to work around that

* There's a fixed number of conditions for each Monitor, giving them a fixed cost, regardless of the number of waiting threads

* Leaving the Monitor (by returning from a monitormethod) triggers reevaluation of the conditions - changes in another Monitor may not wake up this one

* No loop is required to reevaluate the predicate - it is guaranteed to be true if wait() returns without raising an exception

* Only the waiting function specifies which conditions it cares about.  Other functions need not concern themselves with explicitly waking certain threads after they change the state

Additionally, waiting on a condition is a cancellable operation.