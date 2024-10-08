"""Fixer that turns 1L into 1, 0755 into 0o755.
"""
# Copyright 2007 Georg Brandl.
# Licensed to PSF under a Contributor Agreement.

# Local imports
from ..pgen2 import token
from .import basefix
from .util import Number, set


class FixNumliterals(basefix.BaseFix):
    # This is so simple that we don't need the pattern compiler.

    def match(self, node):
        # Override
        return (node.type == token.NUMBER and
                (node.value.startswith("0") or node.value[-1] in "Ll"))

    def transform(self, node, results):
        val = node.value
        if val[-1] in 'Ll':
            val = val[:-1]
        elif val.startswith('0') and val.isdigit() and len(set(val)) > 1:
            val = "0o" + val[1:]

        return Number(val, prefix=node.get_prefix())
