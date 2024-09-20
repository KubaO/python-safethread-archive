"""Tools for working with frozen objects and threads"""
from __future__ import shared_module
# To prevent users from doing "from threadtools import isShareable", we
# use the full operator.isShareable() name
#import operator
from _threadtools import (Monitor, MonitorSpace, MonitorMeta, branch,
    monitormethod, condition, wait)
