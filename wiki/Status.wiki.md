# Status of the project

### (As it was in 2008)

- [ ] weakrefs/finalization
    - [x] weakref.  Callback support is gone.
    - [x] deathqueue, replacing callback functionality
    - [x] `WeakKeyDictionary` and `WeakValueDictionary`, reimplemented using deathqueue
    - [x] `weakref.Proxy`
    - [ ] caching proxies.  Creates a bottleneck, and I allow proxies to non-shareable objects, making implementation more trouble than it's worth.
    - [x] Finalizer class.  This is blocked by threaded imports.
    - [ ] generator cleanup (ie raising `GeneratorExit`)
- [ ] thread-safe imports and shareable modules
    - [x] [`shareddict`](SharedDict.wiki.md)
    - [ ] packages
- [ ] ~~transactions (I'm scrapping them for now)~~
- [ ] Shareability
    - [x] Many things have been marked as shareable.
    - [ ] There's still large gaps though, and several bodges (such as __builtins__).  Warning: the bodges mean it's easy to corrupt and crash the interpreter!
- [ ] [Monitors](Monitors.wiki.md)
    - [x] Core functionality
    - [x] wait function
    - [ ] MonitorSpace.enterlocal() method
    - [x] conditions
    - [x] Deadlock detection
- [x] Cancellation
    - [x] time.sleep()
    - [ ] file reading - works, but bodged
    - [x] conditions
- [x] Tracing GC
- [x]  GIL removal

