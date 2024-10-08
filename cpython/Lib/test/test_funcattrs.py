from test import test_support
import types
import unittest

class FuncAttrsTest(unittest.TestCase):
    def setUp(self):
        class F:
            def a(self):
                pass
        def b():
            return 3
        self.fi = F()
        self.F = F
        self.b = b

    def cannot_set_attr(self, obj, name, value, exceptions):
        try:
            setattr(obj, name, value)
        except exceptions:
            pass
        else:
            self.fail("shouldn't be able to set %s to %r" % (name, value))
        try:
            delattr(obj, name)
        except exceptions:
            pass
        else:
            self.fail("shouldn't be able to del %s" % name)


class FunctionPropertiesTest(FuncAttrsTest):
    # Include the external setUp method that is common to all tests
    def test_module(self):
        self.assertEqual(self.b.__module__, __name__)

    def test_dir_includes_correct_attrs(self):
        self.b.known_attr = 7
        self.assert_('known_attr' in dir(self.b),
            "set attributes not in dir listing of method")
        # Test on underlying function object of method
        self.F.a.known_attr = 7
        self.assert_('known_attr' in dir(self.fi.a), "set attribute on function "
                     "implementations, should show up in next dir")

    def test_duplicate_function_equality(self):
        # Body of `duplicate' is the exact same as self.b
        def duplicate():
            'my docstring'
            return 3
        self.assertNotEqual(self.b, duplicate)

    def test_copying___code__(self):
        def test(): pass
        self.assertEqual(test(), None)
        test.__code__ = self.b.__code__
        self.assertEqual(test(), 3) # self.b always returns 3, arbitrarily

    def test___globals__(self):
        self.assertEqual(self.b.__globals__, globals())
        self.cannot_set_attr(self.b, '__globals__', 2, (AttributeError, TypeError))

    def test___name__(self):
        self.assertEqual(self.b.__name__, 'b')
        self.b.__name__ = 'c'
        self.assertEqual(self.b.__name__, 'c')
        self.b.__name__ = 'd'
        self.assertEqual(self.b.__name__, 'd')
        # __name__ and __name__ must be a string
        self.cannot_set_attr(self.b, '__name__', 7, TypeError)
        # __name__ must be available when in restricted mode. Exec will raise
        # AttributeError if __name__ is not available on f.
        s = """def f(): pass\nf.__name__"""
        exec(s, {'__builtins__': {}})
        # Test on methods, too
        self.assertEqual(self.fi.a.__name__, 'a')
        self.cannot_set_attr(self.fi.a, "__name__", 'a', AttributeError)

    def test___code__(self):
        num_one, num_two = 7, 8
        def a(): pass
        def b(): return 12
        def c(): return num_one
        def d(): return num_two
        def e(): return num_one, num_two
        for func in [a, b, c, d, e]:
            self.assertEqual(type(func.__code__), types.CodeType)
        self.assertEqual(c(), 7)
        self.assertEqual(d(), 8)
        d.__code__ = c.__code__
        self.assertEqual(c.__code__, d.__code__)
        self.assertEqual(c(), 7)
        # self.assertEqual(d(), 7)
        try: b.__code__ = c.__code__
        except ValueError: pass
        else: self.fail(
            "__code__ with different numbers of free vars should not be "
            "possible")
        try: e.__code__ = d.__code__
        except ValueError: pass
        else: self.fail(
            "__code__ with different numbers of free vars should not be "
            "possible")

    def test_blank_func_defaults(self):
        self.assertEqual(self.b.__defaults__, None)
        del self.b.__defaults__
        self.assertEqual(self.b.__defaults__, None)

    def test_func_default_args(self):
        def first_func(a, b):
            return a+b
        def second_func(a=1, b=2):
            return a+b
        self.assertEqual(first_func.__defaults__, None)
        self.assertEqual(second_func.__defaults__, (1, 2))
        first_func.__defaults__ = (1, 2)
        self.assertEqual(first_func.__defaults__, (1, 2))
        self.assertEqual(first_func(), 3)
        self.assertEqual(first_func(3), 5)
        self.assertEqual(first_func(3, 5), 8)
        del second_func.__defaults__
        self.assertEqual(second_func.__defaults__, None)
        try: second_func()
        except TypeError: pass
        else: self.fail(
            "func_defaults does not update; deleting it does not remove "
            "requirement")

class ImplicitReferencesTest(FuncAttrsTest):

    def test___class__(self):
        self.assertEqual(self.fi.a.__self__.__class__, self.F)
        self.cannot_set_attr(self.fi.a, "__class__", self.F, TypeError)

    def test___func__(self):
        self.assertEqual(self.fi.a.__func__, self.F.a)
        self.cannot_set_attr(self.fi.a, "__func__", self.F.a, AttributeError)

    def test___self__(self):
        self.assertEqual(self.fi.a.__self__, self.fi)
        self.cannot_set_attr(self.fi.a, "__self__", self.fi, AttributeError)

    def test___func___non_method(self):
        # Behavior should be the same when a method is added via an attr
        # assignment
        self.fi.id = types.MethodType(id, self.fi)
        self.assertEqual(self.fi.id(), id(self.fi))
        # Test usage
        try: self.fi.id.unknown_attr
        except AttributeError: pass
        else: self.fail("using unknown attributes should raise AttributeError")
        # Test assignment and deletion
        self.cannot_set_attr(self.fi.id, 'unknown_attr', 2, AttributeError)

class ArbitraryFunctionAttrTest(FuncAttrsTest):
    def test_set_attr(self):
        self.b.known_attr = 7
        self.assertEqual(self.b.known_attr, 7)
        try: self.fi.a.known_attr = 7
        except AttributeError: pass
        else: self.fail("setting attributes on methods should raise error")

    def test_delete_unknown_attr(self):
        try: del self.b.unknown_attr
        except AttributeError: pass
        else: self.fail("deleting unknown attribute should raise TypeError")

    def test_unset_attr(self):
        for func in [self.b, self.fi.a]:
            try:  func.non_existant_attr
            except AttributeError: pass
            else: self.fail("using unknown attributes should raise "
                            "AttributeError")

class FunctionDictsTest(FuncAttrsTest):
    def test_setting_dict_to_invalid(self):
        self.cannot_set_attr(self.b, '__dict__', None, TypeError)
        from collections import UserDict
        d = UserDict({'known_attr': 7})
        self.cannot_set_attr(self.fi.a.__func__, '__dict__', d, TypeError)

    def test_setting_dict_to_valid(self):
        d = {'known_attr': 7}
        self.b.__dict__ = d
        # Test assignment
        self.assertEqual(d, self.b.__dict__)
        # ... and on all the different ways of referencing the method's func
        self.F.a.__dict__ = d
        self.assertEqual(d, self.fi.a.__func__.__dict__)
        self.assertEqual(d, self.fi.a.__dict__)
        # Test value
        self.assertEqual(self.b.known_attr, 7)
        self.assertEqual(self.b.__dict__['known_attr'], 7)
        # ... and again, on all the different method's names
        self.assertEqual(self.fi.a.__func__.known_attr, 7)
        self.assertEqual(self.fi.a.known_attr, 7)

    def test_delete___dict__(self):
        try: del self.b.__dict__
        except TypeError: pass
        else: self.fail("deleting function dictionary should raise TypeError")

    def test_unassigned_dict(self):
        self.assertEqual(self.b.__dict__, {})

    def test_func_as_dict_key(self):
        value = "Some string"
        d = {}
        d[self.b] = value
        self.assertEqual(d[self.b], value)

class FunctionDocstringTest(FuncAttrsTest):
    def test_set_docstring_attr(self):
        self.assertEqual(self.b.__doc__, None)
        docstr = "A test method that does nothing"
        self.b.__doc__ = docstr
        self.F.a.__doc__ = docstr
        self.assertEqual(self.b.__doc__, docstr)
        self.assertEqual(self.fi.a.__doc__, docstr)
        self.cannot_set_attr(self.fi.a, "__doc__", docstr, AttributeError)

    def test_delete_docstring(self):
        self.b.__doc__ = "The docstring"
        del self.b.__doc__
        self.assertEqual(self.b.__doc__, None)

def test_main():
    test_support.run_unittest(FunctionPropertiesTest, ImplicitReferencesTest,
                              ArbitraryFunctionAttrTest, FunctionDictsTest,
                              FunctionDocstringTest)

if __name__ == "__main__":
    test_main()
