"""
Manually generated suite used as base class for StdSuites Required and Standard
suites. This is needed because the events and enums in this suite belong
in the Required suite according to the Apple docs, but they often seem to be
in the Standard suite.
"""
import aetools
from . import builtin_Suite


_code_to_module = {
        'reqd' : builtin_Suite,
        'core' : builtin_Suite,
}



_code_to_fullname = {
        'reqd' : ('_builtinSuites.builtin_Suite', 'builtin_Suite'),
        'core' : ('_builtinSuites.builtin_Suite', 'builtin_Suite'),
}

from _builtinSuites.builtin_Suite import *

class _builtinSuites(builtin_Suite_Events,
                aetools.TalkTo):
    _signature = 'ascr'
