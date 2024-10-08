from test import test_support
import time
import unittest


class TimeTestCase(unittest.TestCase):

    def setUp(self):
        self.t = time.time()

    def test_data_attributes(self):
        time.altzone
        time.daylight
        time.timezone
        time.tzname

    def test_clock(self):
        time.clock()

    def test_conversions(self):
        self.assert_(time.ctime(self.t)
                     == time.asctime(time.localtime(self.t)))
        self.assert_(int(time.mktime(time.localtime(self.t)))
                     == int(self.t))

    def test_sleep(self):
        time.sleep(1.2)

    def test_strftime(self):
        tt = time.gmtime(self.t)
        for directive in ('a', 'A', 'b', 'B', 'c', 'd', 'H', 'I',
                          'j', 'm', 'M', 'p', 'S',
                          'U', 'w', 'W', 'x', 'X', 'y', 'Y', 'Z', '%'):
            format = ' %' + directive
            try:
                time.strftime(format, tt)
            except ValueError:
                self.fail('conversion specifier: %r failed.' % format)

    def test_strftime_bounds_checking(self):
        # Make sure that strftime() checks the bounds of the various parts
        #of the time tuple (0 is valid for *all* values).

        # Check year [1900, max(int)]
        self.assertRaises(ValueError, time.strftime, '',
                            (1899, 1, 1, 0, 0, 0, 0, 1, -1))
        if time.accept2dyear:
            self.assertRaises(ValueError, time.strftime, '',
                                (-1, 1, 1, 0, 0, 0, 0, 1, -1))
            self.assertRaises(ValueError, time.strftime, '',
                                (100, 1, 1, 0, 0, 0, 0, 1, -1))
        # Check month [1, 12] + zero support
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, -1, 1, 0, 0, 0, 0, 1, -1))
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 13, 1, 0, 0, 0, 0, 1, -1))
        # Check day of month [1, 31] + zero support
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, -1, 0, 0, 0, 0, 1, -1))
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, 32, 0, 0, 0, 0, 1, -1))
        # Check hour [0, 23]
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, 1, -1, 0, 0, 0, 1, -1))
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, 1, 24, 0, 0, 0, 1, -1))
        # Check minute [0, 59]
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, 1, 0, -1, 0, 0, 1, -1))
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, 1, 0, 60, 0, 0, 1, -1))
        # Check second [0, 61]
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, 1, 0, 0, -1, 0, 1, -1))
        # C99 only requires allowing for one leap second, but Python's docs say
        # allow two leap seconds (0..61)
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, 1, 0, 0, 62, 0, 1, -1))
        # No check for upper-bound day of week;
        #  value forced into range by a ``% 7`` calculation.
        # Start check at -2 since gettmarg() increments value before taking
        #  modulo.
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, 1, 0, 0, 0, -2, 1, -1))
        # Check day of the year [1, 366] + zero support
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, 1, 0, 0, 0, 0, -1, -1))
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, 1, 0, 0, 0, 0, 367, -1))
        # Check daylight savings flag [-1, 1]
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, 1, 0, 0, 0, 0, 1, -2))
        self.assertRaises(ValueError, time.strftime, '',
                            (1900, 1, 1, 0, 0, 0, 0, 1, 2))

    def test_default_values_for_zero(self):
        # Make sure that using all zeros uses the proper default values.
        # No test for daylight savings since strftime() does not change output
        # based on its value.
        expected = "2000 01 01 00 00 00 1 001"
        result = time.strftime("%Y %m %d %H %M %S %w %j", (0,)*9)
        self.assertEquals(expected, result)

    def test_strptime(self):
        # Should be able to go round-trip from strftime to strptime without
        # throwing an exception.
        tt = time.gmtime(self.t)
        for directive in ('a', 'A', 'b', 'B', 'c', 'd', 'H', 'I',
                          'j', 'm', 'M', 'p', 'S',
                          'U', 'w', 'W', 'x', 'X', 'y', 'Y', 'Z', '%'):
            format = '%' + directive
            strf_output = time.strftime(format, tt)
            try:
                time.strptime(strf_output, format)
            except ValueError:
                self.fail("conversion specifier %r failed with '%s' input." %
                          (format, strf_output))

    def test_asctime(self):
        time.asctime(time.gmtime(self.t))
        self.assertRaises(TypeError, time.asctime, 0)

    def test_tzset(self):
        if not hasattr(time, "tzset"):
            return # Can't test this; don't want the test suite to fail

        from os import environ

        # Epoch time of midnight Dec 25th 2002. Never DST in northern
        # hemisphere.
        xmas2002 = 1040774400.0

        # These formats are correct for 2002, and possibly future years
        # This format is the 'standard' as documented at:
        # http://www.opengroup.org/onlinepubs/007904975/basedefs/xbd_chap08.html
        # They are also documented in the tzset(3) man page on most Unix
        # systems.
        eastern = 'EST+05EDT,M4.1.0,M10.5.0'
        victoria = 'AEST-10AEDT-11,M10.5.0,M3.5.0'
        utc='UTC+0'

        org_TZ = environ.get('TZ',None)
        try:
            # Make sure we can switch to UTC time and results are correct
            # Note that unknown timezones default to UTC.
            # Note that altzone is undefined in UTC, as there is no DST
            environ['TZ'] = eastern
            time.tzset()
            environ['TZ'] = utc
            time.tzset()
            self.failUnlessEqual(
                time.gmtime(xmas2002), time.localtime(xmas2002)
                )
            self.failUnlessEqual(time.daylight, 0)
            self.failUnlessEqual(time.timezone, 0)
            self.failUnlessEqual(time.localtime(xmas2002).tm_isdst, 0)

            # Make sure we can switch to US/Eastern
            environ['TZ'] = eastern
            time.tzset()
            self.failIfEqual(time.gmtime(xmas2002), time.localtime(xmas2002))
            self.failUnlessEqual(time.tzname, ('EST', 'EDT'))
            self.failUnlessEqual(len(time.tzname), 2)
            self.failUnlessEqual(time.daylight, 1)
            self.failUnlessEqual(time.timezone, 18000)
            self.failUnlessEqual(time.altzone, 14400)
            self.failUnlessEqual(time.localtime(xmas2002).tm_isdst, 0)
            self.failUnlessEqual(len(time.tzname), 2)

            # Now go to the southern hemisphere.
            environ['TZ'] = victoria
            time.tzset()
            self.failIfEqual(time.gmtime(xmas2002), time.localtime(xmas2002))
            self.failUnless(time.tzname[0] == 'AEST', str(time.tzname[0]))
            self.failUnless(time.tzname[1] == 'AEDT', str(time.tzname[1]))
            self.failUnlessEqual(len(time.tzname), 2)
            self.failUnlessEqual(time.daylight, 1)
            self.failUnlessEqual(time.timezone, -36000)
            self.failUnlessEqual(time.altzone, -39600)
            self.failUnlessEqual(time.localtime(xmas2002).tm_isdst, 1)

        finally:
            # Repair TZ environment variable in case any other tests
            # rely on it.
            if org_TZ is not None:
                environ['TZ'] = org_TZ
            elif 'TZ' in environ:
                del environ['TZ']
            time.tzset()

    def test_insane_timestamps(self):
        # It's possible that some platform maps time_t to double,
        # and that this test will fail there.  This test should
        # exempt such platforms (provided they return reasonable
        # results!).
        for func in time.ctime, time.gmtime, time.localtime:
            for unreasonable in -1e200, 1e200:
                self.assertRaises(ValueError, func, unreasonable)

    def test_ctime_without_arg(self):
        # Not sure how to check the values, since the clock could tick
        # at any time.  Make sure these are at least accepted and
        # don't raise errors.
        time.ctime()
        time.ctime(None)

    def test_gmtime_without_arg(self):
        gt0 = time.gmtime()
        gt1 = time.gmtime(None)
        t0 = time.mktime(gt0)
        t1 = time.mktime(gt1)
        self.assert_(0 <= (t1-t0) < 0.2)

    def test_localtime_without_arg(self):
        lt0 = time.localtime()
        lt1 = time.localtime(None)
        t0 = time.mktime(lt0)
        t1 = time.mktime(lt1)
        self.assert_(0 <= (t1-t0) < 0.2)

def test_main():
    test_support.run_unittest(TimeTestCase)


if __name__ == "__main__":
    test_main()
