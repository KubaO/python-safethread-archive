#!/usr/bin/env python
#
# test_codecencodings_hk.py
#   Codec encoding tests for HongKong encodings.
#

from test import test_support
from test import test_multibytecodec_support
import unittest

class Test_Big5HKSCS(test_multibytecodec_support.TestBase, unittest.TestCase):
    encoding = 'big5hkscs'
    tstring = test_multibytecodec_support.load_teststring('big5hkscs')
    codectests = (
        # invalid bytes
        (b"abc\x80\x80\xc1\xc4", "strict",  None),
        (b"abc\xc8", "strict",  None),
        (b"abc\x80\x80\xc1\xc4", "replace", "abc\ufffd\u8b10"),
        (b"abc\x80\x80\xc1\xc4\xc8", "replace", "abc\ufffd\u8b10\ufffd"),
        (b"abc\x80\x80\xc1\xc4", "ignore",  "abc\u8b10"),
    )

def test_main():
    test_support.run_unittest(__name__)

if __name__ == "__main__":
    test_main()
