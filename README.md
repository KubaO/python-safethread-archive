# python-safethread
## by Adam  Olsen
This is a mirror of the historic python-safethread project from https://code.google.com/archive/p/python-safethread. **It is here to make the project easily browseable for historic preservation.**

The source was originally published as [patches against Python SVN tree](patches/README.md). These patches are each available as a branch of this repository.

| Branch                    | File                                                         | Summary + Labels                                   | Uploaded     | Size      |
| ------------------------- | :----------------------------------------------------------- | :------------------------------------------------- | :----------- | :-------- |
| [bzr-36020](../bzr-36020) | [safethread-bzr-36020.diff](https://storage.googleapis.com/google-code-archive-downloads/v2/code.google.com/python-safethread/safethread-bzr-36020.diff) | patch against 3.0 in bzr, 36020                    | Apr 11, 2008 | 1011.97KB |
| [r61159](../61159)        | [py3k-safethread-r61159.diff](https://storage.googleapis.com/google-code-archive-downloads/v2/code.google.com/python-safethread/py3k-safethread-r61159.diff) | patch against py3k in svn, r61159                  | Mar 13, 2008 | 938.23KB  |
| [r58353-2](../r58353-2)   | [py3k-safethread-r58353-2.diff](https://storage.googleapis.com/google-code-archive-downloads/v2/code.google.com/python-safethread/py3k-safethread-r58353-2.diff) | patch against py3k in svn, r58353, second revision | Jan 30, 2008 | 831.74KB  |
| [r58353](../r58353)       | [py3k-safethread-r58353.diff](https://storage.googleapis.com/google-code-archive-downloads/v2/code.google.com/python-safethread/py3k-safethread-r58353.diff) | patch against py3k in svn, r58353                  | Nov 11, 2007 | 638.06KB  |


https://github.com/KubaO/python-safethread-archive/tree/r58353

Python source is licensed under [PSF License](https://docs.python.org/3/license.html#psf-license). The python-safethread patches/changes are licensed under the Apache License, version 2.0.

---

python-safethread is a large modification of CPython intended to provide safe, easy, and scalable concurrency mechanisms. It focuses on local concurrency, not distributed or parallel programs.

**Note**: *As an implementation, python-safethread is dead. It is not worth the effort to continue rewriting CPython to have a true tracing GC.*

However, the semantics presented are still viable and I intend to reuse them in future projects.

Some major features:

  * Exceptions from threads propagate naturally and cause the program to shut down gracefully.
  * No memory model is necessary. All mutable objects are safely contained with monitors (similar to Concurrent Pascal's monitors, but different from Java's monitors), or otherwise provide explicit semantics.
  * Deadlocks are detected and broken automatically.
  * Finalization is thread-safe (and uses a much simpler mechanism at a low-level.)
  * Most existing single-threaded code will continue to be correct (and in good style) when used amongst threads. Some boilerplate may be necessary to share module and class objects between threads.
  * The GIL is removed. Each additional thread should run at or near 100% throughput. However, the base (single-threaded) throughput is only around 60-65% that of normal CPython, so you'll need several threads for this to be worthwhile.

What little documentation there is:

- [Branching](wiki/Branching.wiki.md)
- [Deadlock Fallacy](wiki/DeadlockFallacy.wiki.md)
- [Finalization](wiki/Finalization.wiki.md)
- [Monitors](wiki/Monitors.wiki.md)
- [Shareability](wiki/Shareability.wiki.md)
- [Shared Dictionaries](wiki/SharedDict.wiki.md)
- [Status](wiki/Status.wiki.md)

---

This mirror is not endorsed by anyone in any way, not even by Adam Olsen :)
