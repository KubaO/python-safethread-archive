# Cleaning up after deleted objects

At a low level, python-safethread only supports weakrefs (no callbacks) and death notices.  A death notice is when you create a death queue and ask it to watch a given object, then you check the queue to see when the object is deleted.  The garbage collector never calls user code, simplifying it greatly.

Although this base mechanism is limited and awkward to use, it provides a basis for more powerful tools.  `object` has been extended to use a `__finalize__` method and a list of attributes specified in `__finalizeattrs__`.  It can be utilized as follows:

```python
from __future__ import shared_module
from threadtools import Monitor, monitormethod

class MyFile(Monitor):
    __shared__ = True  # More shared_module boilerplate, alas
    __finalizeattrs__ = '_fd'

    @monitormethod
    def __finalize__(self):
        super().__finalize__()  # Disables automatic calling if you call __finalize__ manually
        os.close(self._fd)
```

What happens when you create a `MyFile` instance is a _second_ object is created, using it to store `_fd`, while the main object has a property for `_fd` that redirects to this second ("core") object.  When the main object gets deleted the system finalizer thread is notified (using a death queue), and it runs the "core" object's `__finalize__` method.  Since this happens in another thread, `MyFile` instances must be shareable, thus the use of `Monitor` and `monitormethod`.

The core benefit is that any arbitrary attributes will be gone by the time `__finalize__` runs, making unfinalizable cycles much less likely.  If something is unfinalizable, it's because it's still globally reachable through the system death queue.  In the future a tool may be provided to report all reachable objects, and it would indicate any problems here.