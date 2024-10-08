"""
Fixer that changes zip(seq0, seq1, ...) into list(zip(seq0, seq1, ...)
unless there exists a 'from future_builtins import zip' statement in the
top-level namespace.

We avoid the transformation if the zip() call is directly contained in
iter(<>), list(<>), tuple(<>), sorted(<>), ...join(<>), or for V in <>:.
"""

# Local imports
from . import basefix
from .util import Name, Call, in_special_context

class FixZip(basefix.ConditionalFix):

    PATTERN = """
    power< 'zip' args=trailer< '(' [any] ')' >
    >
    """

    skip_on = "future_builtins.zip"

    def transform(self, node, results):
        if self.should_skip(node):
            return

        if in_special_context(node):
            return None

        new = node.clone()
        new.set_prefix("")
        new = Call(Name("list"), [new])
        new.set_prefix(node.get_prefix())
        return new
