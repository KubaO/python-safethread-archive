# test the invariant that
#   iff a==b then hash(a)==hash(b)
#

import unittest
from test import test_support


class HashEqualityTestCase(unittest.TestCase):

    def same_hash(self, *objlist):
        # Hash each object given and fail if
        # the hash values are not all the same.
        hashed = list(map(hash, objlist))
        for h in hashed[1:]:
            if h != hashed[0]:
                self.fail("hashed values differ: %r" % (objlist,))

    def test_numeric_literals(self):
        self.same_hash(1, 1, 1.0, 1.0+0.0j)
        self.same_hash(0, 0.0, 0.0+0.0j)
        self.same_hash(-1, -1.0, -1.0+0.0j)
        self.same_hash(-2, -2.0, -2.0+0.0j)

    def test_coerced_integers(self):
        self.same_hash(int(1), int(1), float(1), complex(1),
                       int('1'), float('1.0'))
        self.same_hash(int(-2**31), float(-2**31))
        self.same_hash(int(1-2**31), float(1-2**31))
        self.same_hash(int(2**31-1), float(2**31-1))
        # for 64-bit platforms
        self.same_hash(int(2**31), float(2**31))
        self.same_hash(int(-2**63), float(-2**63))

    def test_coerced_floats(self):
        self.same_hash(int(1.23e300), float(1.23e300))
        self.same_hash(float(0.5), complex(0.5, 0.0))


def test_main():
    test_support.run_unittest(HashEqualityTestCase)


if __name__ == "__main__":
    test_main()
