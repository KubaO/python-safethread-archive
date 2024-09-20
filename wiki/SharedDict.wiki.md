# Shareable dictionary used as module and class dicts

In the current revision (58353-2), `shareddict` is a mutable dict shared between threads. It may be modified at any time, with the only constraint being that the contents must themselves be shareable. It provides a set of atomic primitive operations, internally implemented using an abort-and-retry mechanism (actually, the same mechanism that CPython already uses.)

Performance wise, shareddict has two behaviours. The starting behaviour assumes a read-write pattern and requires a lock on each access. This allows only one thread to access it at a time (but most operations are split up into several smaller operations.)

However, once a pure-read pattern is detected it switches to a lock-less mode, allowing fully-concurrent access. This allows a module to be used by many threads without contending when they look up globals.

Switching back from pure-read to read-write modes will have a non-trivial performance cost, meaning you should avoid doing occasional writes to a global. I believe this is an acceptable compromise for overall performance, given that it's primarily intended for module and class dicts.