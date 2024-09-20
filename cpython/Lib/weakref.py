"""Weak reference support for Python.

This module is an implementation of PEP 205:

http://python.sourceforge.net/peps/pep-0205.html
"""
from __future__ import shared_module

# XXX FIXME big freakin bug!  MutableMapping.update() does
# "for key in other: self[key] = other[key]".  In the case of a
# WeakValueDictionary, the key may be deleted before you have a chance
# to use it.  Either MutableMapping.update() is wrong, or
# WeakValueDictionary isn't a MutableMapping.  Why doesn't it try
# .items() first?

# Naming convention: Variables named "wr" are weak reference objects;
# they are called this instead of "ref" to avoid name collisions with
# the module-global ref() function imported from _weakref.

import operator
from collections import MutableMapping
from threadtools import Monitor, MonitorMeta

from _weakref import ref, bind, ReferenceType, DeathQueueType, WeakBindingType

from _weakrefset import WeakSet, BypassCallMeta

__all__ = ("ref", "bind", "proxy",
           "ReferenceType", "WeakBindingType", "ProxyType",
           "WeakKeyDictionary", "WeakValueDictionary", "WeakSet")


class WeakValueDictionary(MutableMapping):
    """Mapping class that references values weakly.

    Entries in the dictionary will be discarded when no strong
    reference to the value exists anymore.

    Discarding keys may be delayed until the dictionary is next used.
    """
    __shared__ = True
    # We inherit the constructor without worrying about the input
    # dictionary; since it uses our __setitem__, we get the right checks
    # (if the other dictionary is a WeakValueDictionary, objects are
    # unwrapped on the way out, and we always wrap on the way in).

    def __init__(self, *args, **kw):
        self._deathqueue = DeathQueueType()
        self._data = {}
        self.update(*args, **kw)

    def _checkdeathqueue(self):
        while self._deathqueue:
            hrk = self._deathqueue.pop()
            tmp = self._data.pop(hrk.key)
            assert hrk is tmp

    def __getitem__(self, key):
        self._checkdeathqueue()
        o = self._data[key].ref()
        if o is None:
            raise KeyError(key)
        else:
            return o

    def __setitem__(self, key, value):
        self._checkdeathqueue()
        if key in self._data:
            self._deathqueue.cancel(self._data[key].handle)
        hrk = _HandleRefKey(ref(value), key)
        hrk.handle = self._deathqueue.watch(value, hrk)
        self._data[key] = hrk

    def __delitem__(self, key):
        self._checkdeathqueue()
        hrk = self._data[key]
        del self._data[key]
        self._deathqueue.cancel(hrk.handle)

    def __iter__(self):
        for k in self.keys():
            yield k

    def __len__(self):
        self._checkdeathqueue()
        return len(self._data)

    def __repr__(self):
        return "<WeakValueDictionary at %s>" % id(self)

    def copy(self):
        # XXX Is this really important enough to provide?
        return WeakValueDictionary(self.items())

    def keys(self):
        self._checkdeathqueue()
        # Nearly any operation on us could potentially delete values and
        # invalidate our iteration.  To be safe we always return a set,
        # even though a real dict returns an iterator since 3.0.
        return set(self._data.keys())
    iterkeys = keys

    def items(self):
        self._checkdeathqueue()
        # Nearly any operation on us could potentially delete values and
        # invalidate our iteration.  To be safe we always return a list,
        # even though a real dict returns an iterator since 3.0.
        L = []
        for key, hrk in self._data.items():
            o = hrk.ref()
            if o is not None:
                L.append((key, o))
        return L
    iteritems = items

    def values(self):
        self._checkdeathqueue()
        # Nearly any operation on us could potentially delete values and
        # invalidate our iteration.  To be safe we always return a list,
        # even though a real dict returns an iterator since 3.0.
        L = []
        for hrk in self._data.values():
            o = hrk.ref()
            if o is not None:
                L.append(o)
        return L
    itervalues = values

    def popitem(self):
        self._checkdeathqueue()
        while 1:
            key, hrk = self._data.popitem()
            o = hrk.ref()
            self._deathqueue.cancel(hrk.handle)
            if o is not None:
                return key, o


class _HandleRefKey(Monitor, metaclass=BypassCallMeta):
    __slots__ = "handle", "ref", "key"
    __shared__ = True

    def __init__(self, ref, key):
        self.ref = ref
        self.key = key
        # self.handle has to be filled in after


class WeakKeyDictionary(MutableMapping):
    """ Mapping class that references keys weakly.

    Entries in the dictionary will be discarded when there is no
    longer a strong reference to the key. This can be used to
    associate additional data with an object owned by other parts of
    an application without adding attributes to those objects. This
    can be especially useful with objects that override attribute
    accesses.

    Keys are compared using identity (not value!)

    Discarding values may be delayed until the dictionary is next used.
    """
    __shared__ = True

    def __init__(self, dict=None, **kwargs):
        self._deathqueue = DeathQueueType()
        self._data = {}
        if dict is not None:
            self.update(dict)
        if kwargs:
            self.update(kwargs)

    def _checkdeathqueue(self):
        while self._deathqueue:
            wkhv = self._deathqueue.pop()
            tmp = self._data.pop(wkhv)
            assert wkhv is tmp

    def __getitem__(self, key):
        self._checkdeathqueue()
        key, value = self._data[_WeakKeyHandleValue(key)].binding()
        if key is None:
            raise KeyError(key)
        return value

    def __setitem__(self, key, value):
        self._checkdeathqueue()
        wkhv = _WeakKeyHandleValue(key, value)
        old_wkhv = self._data.get(wkhv, None)
        if old_wkhv is not None:
            del self._data[old_wkhv]
            self._deathqueue.cancel(old_wkhv.handle)

        wkhv.handle = self._deathqueue.watch(key, wkhv)
        self._data[wkhv] = wkhv

    def __delitem__(self, key):
        self._checkdeathqueue()
        wkhv = self._data[_WeakKeyHandleValue(key)]
        del self._data[wkhv]
        self._deathqueue.cancel(wkhv.handle)

    def __iter__(self):
        for k in self.keys():
            yield k

    def __len__(self):
        self._checkdeathqueue()
        return len(self._data)

    def __repr__(self):
        return "<WeakKeyDictionary at %s>" % id(self)

    def copy(self):
        # XXX Is this really important enough to provide?
        return WeakKeyDictionary(self.items())

    def keys(self):
        self._checkdeathqueue()
        # Nearly any operation on us could potentially delete keys and
        # invalidate our iteration.  To be safe we always return a set,
        # even though a real dict returns an iterator since 3.0.
        s = set()
        for wkhv in self._data:
            key, value = wkhv.binding()
            if key is not None:
                s.add(key)
        return s
    iterkeys = keys

    def items(self):
        self._checkdeathqueue()
        # Nearly any operation on us could potentially delete keys and
        # invalidate our iteration.  To be safe we always return a list,
        # even though a real dict returns an iterator since 3.0.
        L = []
        for wkhv in self._data:
            key, value = wkhv.binding()
            if key is not None:
                L.append((key, value))
        return L
    iteritems = items

    def values(self):
        self._checkdeathqueue()
        # Nearly any operation on us could potentially delete keys and
        # invalidate our iteration.  To be safe we always return a list,
        # even though a real dict returns an iterator since 3.0.
        L = []
        for wkhv in self._data:
            key, value = wkhv.binding()
            if key is not None:
                L.append(value)
        return L
    itervalues = values

    def popitem(self):
        self._checkdeathqueue()
        while 1:
            wkhv, wkhv = self._data.popitem()
            key, value = wkhv.binding()
            self._deathqueue.cancel(wkhv.handle)
            if key is not None:
                return key, value


class _WeakKeyHandleValue(Monitor, metaclass=BypassCallMeta):
    __slots__ = 'binding', 'handle', 'hash'
    __shared__ = True

    def __init__(self, key, value=None):
        self.binding = bind(key, value)
        self.hash = hash(id(key))
        # self.handle has to be filled in after

    def __hash__(self):
        return self.hash

    def __eq__(self, other):
        if not isinstance(other, _WeakKeyHandleValue):
            return NotImplemented

        # We guarantee a _WeakKeyHandleValue always compares equal to
        # itself, even if the weakref has been cleared.
        if self is other:
            return True

        # If it's a different _WeakKeyHandleValue then we compare
        # objects, defaulting to False if either of them have been
        # cleared.
        self_o, self_value = self.binding()
        other_o, other_value = other.binding()
        if self_o is None or other_o is None:
            return False
        return self_o is other_o


def proxy(obj):
    return ProxyType(obj)


class ProxyType:
    __shared__ = True

    def __init__(self, obj):
        super().__setattr__('ref', ref(obj))

    def __getattribute__(self, name):
        o = super().__getattribute__('ref')()
        if o is None:
            raise ReferenceError
        return getattr(o, name)

    def __repr__(self):
        o = super().__getattribute__('ref')()
        if o is None:
            return '<weakproxy at {0}, dead>'.format(hex(id(self)))
        return '<weakproxy at {0} to {1} at {2}>'.format(hex(id(self)),
            type(o).__name__, hex(id(o)))

    def __bool__(self):
        # We can't use our generic operator proxying because the names
        # are different.  __bool__ -> operator.truth
        o = super().__getattribute__('ref')()
        if o is None:
            raise ReferenceError
        return bool(o)

CallableProxyType = ProxyType

_directmethods = tuple("""
setattr delattr call iter len str
""".split())
def _directproxy_(name):
    name = '__{0}__'.format(name)
    def _directproxy(self, *args, **kwargs):
        o = super(ProxyType, self).__getattribute__('ref')()
        if o is None:
            raise ReferenceError
        return getattr(o, name)(*args, **kwargs)
    return _directproxy
for _directname in _directmethods:
    _name = '__' + _directname + '__'
    setattr(ProxyType, _name, _directproxy_(_directname))

_opermethods = tuple("""
abs add and concat contains delitem delslice eq floordiv ge getitem
getslice gt iadd iand iconcat ifloordiv ilshift imod imul index inv
invert ior ipow irepeat irshift isub itruediv ixor le lshift lt mod mul
ne neg not or pos pow repeat rshift setitem setslice sub truediv xor
""".split())
def _operproxy_(name):
    oper = getattr(operator, '__{0}__'.format(name))
    def _operproxy(self, *args):
        o = super(ProxyType, self).__getattribute__('ref')()
        if o is None:
            raise ReferenceError
        return oper(o, *args)
    return _operproxy
for _opername in _opermethods:
    _name = '__' + _opername + '__'
    setattr(ProxyType, _name, _operproxy_(_opername))

_reverseopermethods = tuple("""
add sub mul
truediv floordiv mod pow
lshift rshift and xor or
""".split())
def _reverseoperproxy_(name):
    oper = getattr(operator, '__{0}__'.format(name))
    def _operproxy(self, other):
        o = super(ProxyType, self).__getattribute__('ref')()
        if o is None:
            raise ReferenceError
        return oper(other, o)
    return _operproxy
for _opername in _reverseopermethods:
    _name = '__r' + _opername + '__'
    setattr(ProxyType, _name, _reverseoperproxy_(_opername))
