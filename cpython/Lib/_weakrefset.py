from __future__ import shared_module

# Access WeakSet through the weakref module.
# This code is separated-out because it is needed
# by abc.py to load everything else at startup.

from _weakref import ref, DeathQueueType
from threadtools import Monitor, MonitorMeta

__all__ = ('WeakSet',)


class BypassCallMeta(MonitorMeta):
    __shared__ = True

    def __call__(self, *args, **kwargs):
        # We bypass MonitorMeta.__call__, so that no MonitorSpace is
        # created.  This is perverse, but it works.
        return type.__call__(self, *args, **kwargs)


class WeakSet:
    __shared__ = True

    def __init__(self, data=None):
        self._deathqueue = DeathQueueType()
        self._data = {}
        if data is not None:
            self.update(data)

    def _checkdeathqueue(self):
        while self._deathqueue:
            entry = self._deathqueue.pop()
            tmp = self._data.pop(entry)
            assert entry is tmp

    def __iter__(self):
        self._checkdeathqueue()
        for entry in self._data:
            item = entry.ref()
            if item is not None:
                yield item

    def __contains__(self, item):
        self._checkdeathqueue()
        return _Entry(item) in self._data

    def __reduce__(self):
        d = getattr(self, '__dict__', {})
        d.pop('_data', None)
        d.pop('_deathqueue', None)
        return (self.__class__, (list(self),), d)

    def add(self, item):
        olditem = self.intern(item)
        if olditem is not item:
            raise ValueError("WeakSet.add requires equal items to have the same identity")

    def intern(self, item):
        self._checkdeathqueue()
        entry = _Entry(item)

        try:
            oldentry = self._data[entry]
        except KeyError:
            pass
        else:
            olditem = oldentry.ref()
            if olditem is None:
                del self._data[oldentry]
                self._deathqueue.cancel(oldentry.handle)
            else:
                return olditem

        entry.handle = self._deathqueue.watch(item, entry)
        self._data[entry] = entry
        return item

    def clear(self):
        try:
            while True:
                self.pop()
        except KeyError:
            pass

    def copy(self):
        return self.__class__(self)

    def pop(self):
        self._checkdeathqueue()
        while True:
            entry = self._data.pop()
            item = entry.ref()
            if item is None:
                self._deathqueue.cancel(entry.handle)
            else:
                return item

    def remove(self, item):
        self._checkdeathqueue()
        entry = self._data[_Entry(item)]
        del self._data[entry]
        self._deathqueue.cancel(entry.handle)

    def discard(self, item):
        try:
            self.remove(item)
        except KeyError:
            pass

    def update(self, other):
        for item in other:
            self.add(item)

    def __ior__(self, other):
        self.update(other)
        return self

    def _proxy_set(methname):
        method = getattr(set, methname)
        def func(self, other):
            return self.__class__(method(set(self), other))
        func.func_name = methname
        return func

    def _proxy_set_mutate(methname):
        method = getattr(set, methname)
        def func(self, other):
            s = set(self)
            method(s, other)
            self.clear()
            self.update(s)
        func.func_name = methname
        return func

    def _proxy_set_mutateoper(methname):
        method = getattr(set, methname)
        def func(self, other):
            s = set(self)
            method(s, other)
            self.clear()
            self.update(s)
            return self
        func.func_name = methname
        return func

    difference = _proxy_set('difference')
    intersection = _proxy_set('intersection')
    issubset = _proxy_set('issubset')
    issuperset = _proxy_set('issuperset')
    symmetric_difference = _proxy_set('symmetric_difference')
    union = _proxy_set('union')
    difference_update = _proxy_set_mutate('difference_update')
    intersection_update = _proxy_set_mutate('intersection_update')
    symmetric_difference_update = _proxy_set_mutate('symmetric_difference_update')
    __sub__ = _proxy_set('__sub__')
    __and__ = _proxy_set('__and__')
    __lt__ = _proxy_set('__lt__')
    __gt__ = _proxy_set('__gt__')
    __xor__ = _proxy_set('__xor__')
    __or__ = _proxy_set('__or__')
    __isub__ = _proxy_set_mutateoper('__isub__')
    __iand__ = _proxy_set_mutateoper('__iand__')
    __ixor__ = _proxy_set_mutateoper('__ixor__')

    del _proxy_set, _proxy_set_mutate, _proxy_set_mutateoper


class _Entry(Monitor, metaclass=BypassCallMeta):
    __slots__ = 'ref', 'handle', 'hash'
    __shared__ = True

    def __init__(self, item):
        self.ref = ref(item)
        self.hash = hash(item)

    def __hash__(self):
        return self.hash

    def __eq__(self, other):
        if not isinstance(other, _Entry):
            return NotImplemented

        # We guarantee a _Entry always compares equal to itself, even if
        # the weakref has been cleared.
        if self is other:
            return True

        # If it's a different _Entry then we compare objects, defaulting
        # to False if either of them have been cleared.
        self_item = self.ref()
        other_item = other.ref()
        if self_item is None or other_item is None:
            return False
        return self_item == other_item


class WeakSetOld:
    __shared__ = True

    def __init__(self, data=None):
        self.data = set()
        def _remove(item, selfref=ref(self)):
            self = selfref()
            if self is not None:
                self.data.discard(item)
        self._remove = _remove
        if data is not None:
            self.update(data)

    def __iter__(self):
        for itemref in self.data:
            item = itemref()
            if item is not None:
                yield item

    def __contains__(self, item):
        return ref(item) in self.data

    def __reduce__(self):
        return (self.__class__, (list(self),),
                getattr(self, '__dict__', None))

    def add(self, item):
        self.data.add(ref(item, self._remove))

    def clear(self):
        self.data.clear()

    def copy(self):
        return self.__class__(self)

    def pop(self):
        while True:
            itemref = self.data.pop()
            item = itemref()
            if item is not None:
                return item

    def remove(self, item):
        self.data.remove(ref(item))

    def discard(self, item):
        self.data.discard(ref(item))

    def update(self, other):
        if isinstance(other, self.__class__):
            self.data.update(other.data)
        else:
            for element in other:
                self.add(element)
    __ior__ = update

    # Helper functions for simple delegating methods.
    def _apply(self, other, method):
        if not isinstance(other, self.__class__):
            other = self.__class__(other)
        newdata = method(other.data)
        newset = self.__class__()
        newset.data = newdata
        return newset

    def _apply_mutate(self, other, method):
        if not isinstance(other, self.__class__):
            other = self.__class__(other)
        method(other)

    def difference(self, other):
        return self._apply(other, self.data.difference)
    __sub__ = difference

    def difference_update(self, other):
        self._apply_mutate(self, self.data.difference_update)
    __isub__ = difference_update

    def intersection(self, other):
        return self._apply(other, self.data.intersection)
    __and__ = intersection

    def intersection_update(self, other):
        self._apply_mutate(self, self.data.intersection_update)
    __iand__ = intersection_update

    def issubset(self, other):
        return self.data.issubset(ref(item) for item in other)
    __lt__ = issubset

    def issuperset(self, other):
        return self.data.issuperset(ref(item) for item in other)
    __gt__ = issuperset

    def symmetric_difference(self, other):
        return self._apply(other, self.data.symmetric_difference)
    __xor__ = symmetric_difference

    def symmetric_difference_update(self, other):
        self._apply_mutate(other, self.data.symmetric_difference_update)
    __ixor__ = symmetric_difference_update

    def union(self, other):
        self._apply_mutate(other, self.data.union)
    __or__ = union
