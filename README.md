# python-safethread
## by Adam  Olsen
This is a mirror of the historic python-safethread project from https://code.google.com/archive/p/python-safethread.

**It is here to make the project easily browseable for historic preservation.**

The source is available here as [patches against Python SVN tree](patches/README.md). PRs of the 4 SVN Python revision checkouts, each in its own branch, with a patch applied as a separate commit, are welcome.

Licensed under the [PSF License](https://docs.python.org/3/license.html#psf-license) as a derivative work of Python.

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
