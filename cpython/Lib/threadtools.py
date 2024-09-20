"""Tools for working with frozen objects and threads"""
from __future__ import shared_module
# To prevent users from doing "from threadtools import isShareable", we
# use the full operator.isShareable() name
#import operator
from _threadtools import Monitor, MonitorSpace, MonitorMeta, branch

def monitormethod(func):
    # XXX FIXME _monitormethod shouldn't be shareable (because it has a
    # closure), but it seems to be marked as shareable anyway!
    def _monitormethod(self, *args, **kwargs):
        return self.enter(func, self, *args, **kwargs)
    return _monitormethod
