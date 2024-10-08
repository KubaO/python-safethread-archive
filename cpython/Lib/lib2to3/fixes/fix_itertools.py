""" Fixer for itertools.(imap|ifilter|izip) --> (map|filter|zip) and
    itertools.ifilterfalse --> itertools.filterfalse (bugs 2360-2363)

    imports from itertools are fixed in fix_itertools_import.py

    If itertools is imported as something else (ie: import itertools as it;
    it.izip(spam, eggs)) method calls will not get fixed.
    """

# Local imports
from . import basefix
from .util import Name

class FixItertools(basefix.BaseFix):
    it_funcs = "('imap'|'ifilter'|'izip'|'ifilterfalse')"
    PATTERN = """
              power< it='itertools'
                  trailer<
                     dot='.' func=%(it_funcs)s > trailer< '(' [any] ')' > >
              |
              power< func=%(it_funcs)s trailer< '(' [any] ')' > >
              """ %(locals())

    # Needs to be run after fix_(map|zip|filter)
    run_order = 6

    def transform(self, node, results):
        prefix = None
        func = results['func'][0]
        if 'it' in results and func.value != 'ifilterfalse':
            dot, it = (results['dot'], results['it'])
            # Remove the 'itertools'
            prefix = it.get_prefix()
            it.remove()
            # Replace the node wich contains ('.', 'function') with the
            # function (to be consistant with the second part of the pattern)
            dot.remove()
            func.parent.replace(func)

        prefix = prefix or func.get_prefix()
        func.replace(Name(func.value[1:], prefix=prefix))
