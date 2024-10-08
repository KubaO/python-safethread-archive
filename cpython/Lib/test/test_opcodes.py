# Python test set -- part 2, opcodes

from test.test_support import run_unittest
import unittest

class OpcodeTest(unittest.TestCase):

    def test_try_inside_for_loop(self):
        n = 0
        for i in range(10):
            n = n+i
            try: 1/0
            except NameError: pass
            except ZeroDivisionError: pass
            except TypeError: pass
            try: pass
            except: pass
            try: pass
            finally: pass
            n = n+i
        if n != 90:
            self.fail('try inside for')

    def test_raise_class_exceptions(self):

        class AClass(Exception): pass
        class BClass(AClass): pass
        class CClass(Exception): pass
        class DClass(AClass):
            def __init__(self, ignore):
                pass

        try: raise AClass()
        except: pass

        try: raise AClass()
        except AClass: pass

        try: raise BClass()
        except AClass: pass

        try: raise BClass()
        except CClass: self.fail()
        except: pass

        a = AClass()
        b = BClass()

        try:
            raise b
        except AClass as v:
            self.assertEqual(v, b)
        else:
            self.fail("no exception")

        # not enough arguments
        ##try:  raise BClass, a
        ##except TypeError: pass
        ##else: self.fail("no exception")

        try:  raise DClass(a)
        except DClass as v:
            self.assert_(isinstance(v, DClass))
        else:
            self.fail("no exception")

    def test_compare_function_objects(self):

        f = eval('lambda: None')
        g = eval('lambda: None')
        self.failIf(f == g)

        f = eval('lambda a: a')
        g = eval('lambda a: a')
        self.failIf(f == g)

        f = eval('lambda a=1: a')
        g = eval('lambda a=1: a')
        self.failIf(f == g)

        f = eval('lambda: 0')
        g = eval('lambda: 1')
        self.failIf(f == g)

        f = eval('lambda: None')
        g = eval('lambda a: None')
        self.failIf(f == g)

        f = eval('lambda a: None')
        g = eval('lambda b: None')
        self.failIf(f == g)

        f = eval('lambda a: None')
        g = eval('lambda a=None: None')
        self.failIf(f == g)

        f = eval('lambda a=0: None')
        g = eval('lambda a=1: None')
        self.failIf(f == g)


def test_main():
    run_unittest(OpcodeTest)

if __name__ == '__main__':
    test_main()
