/*  C implementation for the date/time type documented at
 *  http://www.zope.org/Members/fdrake/DateTimeWiki/FrontPage
 */

#include "Python.h"
#include "modsupport.h"
#include "structmember.h"

#include <time.h>

#include "timefuncs.h"

/* Differentiate between building the core module and building extension
 * modules.
 */
#ifndef Py_BUILD_CORE
#define Py_BUILD_CORE
#endif
#include "datetime.h"
#undef Py_BUILD_CORE

/* We require that C int be at least 32 bits, and use int virtually
 * everywhere.  In just a few cases we use a temp long, where a Python
 * API returns a C long.  In such cases, we have to ensure that the
 * final result fits in a C int (this can be an issue on 64-bit boxes).
 */
#if SIZEOF_INT < 4
#	error "datetime.c requires that C int have at least 32 bits"
#endif

#define MINYEAR 1
#define MAXYEAR 9999

/* Nine decimal digits is easy to communicate, and leaves enough room
 * so that two delta days can be added w/o fear of overflowing a signed
 * 32-bit int, and with plenty of room left over to absorb any possible
 * carries from adding seconds.
 */
#define MAX_DELTA_DAYS 999999999

/* Rename the long macros in datetime.h to more reasonable short names. */
#define GET_YEAR		PyDateTime_GET_YEAR
#define GET_MONTH		PyDateTime_GET_MONTH
#define GET_DAY			PyDateTime_GET_DAY
#define DATE_GET_HOUR		PyDateTime_DATE_GET_HOUR
#define DATE_GET_MINUTE		PyDateTime_DATE_GET_MINUTE
#define DATE_GET_SECOND		PyDateTime_DATE_GET_SECOND
#define DATE_GET_MICROSECOND	PyDateTime_DATE_GET_MICROSECOND

/* Date accessors for date and datetime. */
#define SET_YEAR(o, v)		(((o)->data[0] = ((v) & 0xff00) >> 8), \
                                 ((o)->data[1] = ((v) & 0x00ff)))
#define SET_MONTH(o, v)		(PyDateTime_GET_MONTH(o) = (v))
#define SET_DAY(o, v)		(PyDateTime_GET_DAY(o) = (v))

/* Date/Time accessors for datetime. */
#define DATE_SET_HOUR(o, v)	(PyDateTime_DATE_GET_HOUR(o) = (v))
#define DATE_SET_MINUTE(o, v)	(PyDateTime_DATE_GET_MINUTE(o) = (v))
#define DATE_SET_SECOND(o, v)	(PyDateTime_DATE_GET_SECOND(o) = (v))
#define DATE_SET_MICROSECOND(o, v)	\
	(((o)->data[7] = ((v) & 0xff0000) >> 16), \
         ((o)->data[8] = ((v) & 0x00ff00) >> 8), \
         ((o)->data[9] = ((v) & 0x0000ff)))

/* Time accessors for time. */
#define TIME_GET_HOUR		PyDateTime_TIME_GET_HOUR
#define TIME_GET_MINUTE		PyDateTime_TIME_GET_MINUTE
#define TIME_GET_SECOND		PyDateTime_TIME_GET_SECOND
#define TIME_GET_MICROSECOND	PyDateTime_TIME_GET_MICROSECOND
#define TIME_SET_HOUR(o, v)	(PyDateTime_TIME_GET_HOUR(o) = (v))
#define TIME_SET_MINUTE(o, v)	(PyDateTime_TIME_GET_MINUTE(o) = (v))
#define TIME_SET_SECOND(o, v)	(PyDateTime_TIME_GET_SECOND(o) = (v))
#define TIME_SET_MICROSECOND(o, v)	\
	(((o)->data[3] = ((v) & 0xff0000) >> 16), \
         ((o)->data[4] = ((v) & 0x00ff00) >> 8), \
         ((o)->data[5] = ((v) & 0x0000ff)))

/* Delta accessors for timedelta. */
#define GET_TD_DAYS(o)		(((PyDateTime_Delta *)(o))->days)
#define GET_TD_SECONDS(o)	(((PyDateTime_Delta *)(o))->seconds)
#define GET_TD_MICROSECONDS(o)	(((PyDateTime_Delta *)(o))->microseconds)

#define SET_TD_DAYS(o, v)	((o)->days = (v))
#define SET_TD_SECONDS(o, v)	((o)->seconds = (v))
#define SET_TD_MICROSECONDS(o, v) ((o)->microseconds = (v))

/* p is a pointer to a time or a datetime object; HASTZINFO(p) returns
 * p->hastzinfo.
 */
#define HASTZINFO(p)		(((_PyDateTime_BaseTZInfo *)(p))->hastzinfo)

/* M is a char or int claiming to be a valid month.  The macro is equivalent
 * to the two-sided Python test
 *	1 <= M <= 12
 */
#define MONTH_IS_SANE(M) ((unsigned int)(M) - 1 < 12)

/* Forward declarations. */
static PyTypeObject PyDateTime_DateType;
static PyTypeObject PyDateTime_DateTimeType;
static PyTypeObject PyDateTime_DeltaType;
static PyTypeObject PyDateTime_TimeType;
static PyTypeObject PyDateTime_TZInfoType;

/* ---------------------------------------------------------------------------
 * Math utilities.
 */

/* k = i+j overflows iff k differs in sign from both inputs,
 * iff k^i has sign bit set and k^j has sign bit set,
 * iff (k^i)&(k^j) has sign bit set.
 */
#define SIGNED_ADD_OVERFLOWED(RESULT, I, J) \
	((((RESULT) ^ (I)) & ((RESULT) ^ (J))) < 0)

/* Compute Python divmod(x, y), returning the quotient and storing the
 * remainder into *r.  The quotient is the floor of x/y, and that's
 * the real point of this.  C will probably truncate instead (C99
 * requires truncation; C89 left it implementation-defined).
 * Simplification:  we *require* that y > 0 here.  That's appropriate
 * for all the uses made of it.  This simplifies the code and makes
 * the overflow case impossible (divmod(LONG_MIN, -1) is the only
 * overflow case).
 */
static int
divmod(int x, int y, int *r)
{
	int quo;

	assert(y > 0);
	quo = x / y;
	*r = x - quo * y;
	if (*r < 0) {
		--quo;
		*r += y;
	}
	assert(0 <= *r && *r < y);
	return quo;
}

/* Round a double to the nearest long.  |x| must be small enough to fit
 * in a C long; this is not checked.
 */
static long
round_to_long(double x)
{
	if (x >= 0.0)
		x = floor(x + 0.5);
	else
		x = ceil(x - 0.5);
	return (long)x;
}

/* ---------------------------------------------------------------------------
 * General calendrical helper functions
 */

/* For each month ordinal in 1..12, the number of days in that month,
 * and the number of days before that month in the same year.  These
 * are correct for non-leap years only.
 */
static int _days_in_month[] = {
	0, /* unused; this vector uses 1-based indexing */
	31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int _days_before_month[] = {
	0, /* unused; this vector uses 1-based indexing */
	0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

/* year -> 1 if leap year, else 0. */
static int
is_leap(int year)
{
	/* Cast year to unsigned.  The result is the same either way, but
	 * C can generate faster code for unsigned mod than for signed
	 * mod (especially for % 4 -- a good compiler should just grab
	 * the last 2 bits when the LHS is unsigned).
	 */
	const unsigned int ayear = (unsigned int)year;
	return ayear % 4 == 0 && (ayear % 100 != 0 || ayear % 400 == 0);
}

/* year, month -> number of days in that month in that year */
static int
days_in_month(int year, int month)
{
	assert(month >= 1);
	assert(month <= 12);
	if (month == 2 && is_leap(year))
		return 29;
	else
		return _days_in_month[month];
}

/* year, month -> number of days in year preceeding first day of month */
static int
days_before_month(int year, int month)
{
	int days;

	assert(month >= 1);
	assert(month <= 12);
	days = _days_before_month[month];
	if (month > 2 && is_leap(year))
		++days;
	return days;
}

/* year -> number of days before January 1st of year.  Remember that we
 * start with year 1, so days_before_year(1) == 0.
 */
static int
days_before_year(int year)
{
	int y = year - 1;
	/* This is incorrect if year <= 0; we really want the floor
	 * here.  But so long as MINYEAR is 1, the smallest year this
	 * can see is 0 (this can happen in some normalization endcases),
	 * so we'll just special-case that.
	 */
	assert (year >= 0);
	if (y >= 0)
		return y*365 + y/4 - y/100 + y/400;
	else {
		assert(y == -1);
		return -366;
	}
}

/* Number of days in 4, 100, and 400 year cycles.  That these have
 * the correct values is asserted in the module init function.
 */
#define DI4Y	1461	/* days_before_year(5); days in 4 years */
#define DI100Y	36524	/* days_before_year(101); days in 100 years */
#define DI400Y	146097	/* days_before_year(401); days in 400 years  */

/* ordinal -> year, month, day, considering 01-Jan-0001 as day 1. */
static void
ord_to_ymd(int ordinal, int *year, int *month, int *day)
{
	int n, n1, n4, n100, n400, leapyear, preceding;

	/* ordinal is a 1-based index, starting at 1-Jan-1.  The pattern of
	 * leap years repeats exactly every 400 years.  The basic strategy is
	 * to find the closest 400-year boundary at or before ordinal, then
	 * work with the offset from that boundary to ordinal.  Life is much
	 * clearer if we subtract 1 from ordinal first -- then the values
	 * of ordinal at 400-year boundaries are exactly those divisible
	 * by DI400Y:
	 *
	 *    D  M   Y            n              n-1
	 *    -- --- ----        ----------     ----------------
	 *    31 Dec -400        -DI400Y       -DI400Y -1
	 *     1 Jan -399         -DI400Y +1   -DI400Y      400-year boundary
	 *    ...
	 *    30 Dec  000        -1             -2
	 *    31 Dec  000         0             -1
	 *     1 Jan  001         1              0          400-year boundary
	 *     2 Jan  001         2              1
	 *     3 Jan  001         3              2
	 *    ...
	 *    31 Dec  400         DI400Y        DI400Y -1
	 *     1 Jan  401         DI400Y +1     DI400Y      400-year boundary
	 */
	assert(ordinal >= 1);
	--ordinal;
	n400 = ordinal / DI400Y;
	n = ordinal % DI400Y;
	*year = n400 * 400 + 1;

	/* Now n is the (non-negative) offset, in days, from January 1 of
	 * year, to the desired date.  Now compute how many 100-year cycles
	 * precede n.
	 * Note that it's possible for n100 to equal 4!  In that case 4 full
	 * 100-year cycles precede the desired day, which implies the
	 * desired day is December 31 at the end of a 400-year cycle.
	 */
	n100 = n / DI100Y;
	n = n % DI100Y;

	/* Now compute how many 4-year cycles precede it. */
	n4 = n / DI4Y;
	n = n % DI4Y;

	/* And now how many single years.  Again n1 can be 4, and again
	 * meaning that the desired day is December 31 at the end of the
	 * 4-year cycle.
	 */
	n1 = n / 365;
	n = n % 365;

	*year += n100 * 100 + n4 * 4 + n1;
	if (n1 == 4 || n100 == 4) {
		assert(n == 0);
		*year -= 1;
		*month = 12;
		*day = 31;
		return;
	}

	/* Now the year is correct, and n is the offset from January 1.  We
	 * find the month via an estimate that's either exact or one too
	 * large.
	 */
	leapyear = n1 == 3 && (n4 != 24 || n100 == 3);
	assert(leapyear == is_leap(*year));
	*month = (n + 50) >> 5;
	preceding = (_days_before_month[*month] + (*month > 2 && leapyear));
	if (preceding > n) {
		/* estimate is too large */
		*month -= 1;
		preceding -= days_in_month(*year, *month);
	}
	n -= preceding;
	assert(0 <= n);
	assert(n < days_in_month(*year, *month));

	*day = n + 1;
}

/* year, month, day -> ordinal, considering 01-Jan-0001 as day 1. */
static int
ymd_to_ord(int year, int month, int day)
{
	return days_before_year(year) + days_before_month(year, month) + day;
}

/* Day of week, where Monday==0, ..., Sunday==6.  1/1/1 was a Monday. */
static int
weekday(int year, int month, int day)
{
	return (ymd_to_ord(year, month, day) + 6) % 7;
}

/* Ordinal of the Monday starting week 1 of the ISO year.  Week 1 is the
 * first calendar week containing a Thursday.
 */
static int
iso_week1_monday(int year)
{
	int first_day = ymd_to_ord(year, 1, 1);	/* ord of 1/1 */
	/* 0 if 1/1 is a Monday, 1 if a Tue, etc. */
	int first_weekday = (first_day + 6) % 7;
	/* ordinal of closest Monday at or before 1/1 */
	int week1_monday  = first_day - first_weekday;

	if (first_weekday > 3)	/* if 1/1 was Fri, Sat, Sun */
		week1_monday += 7;
	return week1_monday;
}

/* ---------------------------------------------------------------------------
 * Range checkers.
 */

/* Check that -MAX_DELTA_DAYS <= days <= MAX_DELTA_DAYS.  If so, return 0.
 * If not, raise OverflowError and return -1.
 */
static int
check_delta_day_range(int days)
{
	if (-MAX_DELTA_DAYS <= days && days <= MAX_DELTA_DAYS)
		return 0;
	PyErr_Format(PyExc_OverflowError,
		     "days=%d; must have magnitude <= %d",
		     days, MAX_DELTA_DAYS);
	return -1;
}

/* Check that date arguments are in range.  Return 0 if they are.  If they
 * aren't, raise ValueError and return -1.
 */
static int
check_date_args(int year, int month, int day)
{

	if (year < MINYEAR || year > MAXYEAR) {
		PyErr_SetString(PyExc_ValueError,
				"year is out of range");
		return -1;
	}
	if (month < 1 || month > 12) {
		PyErr_SetString(PyExc_ValueError,
				"month must be in 1..12");
		return -1;
	}
	if (day < 1 || day > days_in_month(year, month)) {
		PyErr_SetString(PyExc_ValueError,
				"day is out of range for month");
		return -1;
	}
	return 0;
}

/* Check that time arguments are in range.  Return 0 if they are.  If they
 * aren't, raise ValueError and return -1.
 */
static int
check_time_args(int h, int m, int s, int us)
{
	if (h < 0 || h > 23) {
		PyErr_SetString(PyExc_ValueError,
				"hour must be in 0..23");
		return -1;
	}
	if (m < 0 || m > 59) {
		PyErr_SetString(PyExc_ValueError,
				"minute must be in 0..59");
		return -1;
	}
	if (s < 0 || s > 59) {
		PyErr_SetString(PyExc_ValueError,
				"second must be in 0..59");
		return -1;
	}
	if (us < 0 || us > 999999) {
		PyErr_SetString(PyExc_ValueError,
				"microsecond must be in 0..999999");
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * Normalization utilities.
 */

/* One step of a mixed-radix conversion.  A "hi" unit is equivalent to
 * factor "lo" units.  factor must be > 0.  If *lo is less than 0, or
 * at least factor, enough of *lo is converted into "hi" units so that
 * 0 <= *lo < factor.  The input values must be such that int overflow
 * is impossible.
 */
static void
normalize_pair(int *hi, int *lo, int factor)
{
	assert(factor > 0);
	assert(lo != hi);
	if (*lo < 0 || *lo >= factor) {
		const int num_hi = divmod(*lo, factor, lo);
		const int new_hi = *hi + num_hi;
		assert(! SIGNED_ADD_OVERFLOWED(new_hi, *hi, num_hi));
		*hi = new_hi;
	}
	assert(0 <= *lo && *lo < factor);
}

/* Fiddle days (d), seconds (s), and microseconds (us) so that
 * 	0 <= *s < 24*3600
 * 	0 <= *us < 1000000
 * The input values must be such that the internals don't overflow.
 * The way this routine is used, we don't get close.
 */
static void
normalize_d_s_us(int *d, int *s, int *us)
{
	if (*us < 0 || *us >= 1000000) {
		normalize_pair(s, us, 1000000);
		/* |s| can't be bigger than about
		 * |original s| + |original us|/1000000 now.
		 */

	}
	if (*s < 0 || *s >= 24*3600) {
		normalize_pair(d, s, 24*3600);
		/* |d| can't be bigger than about
		 * |original d| +
		 * (|original s| + |original us|/1000000) / (24*3600) now.
		 */
	}
	assert(0 <= *s && *s < 24*3600);
	assert(0 <= *us && *us < 1000000);
}

/* Fiddle years (y), months (m), and days (d) so that
 * 	1 <= *m <= 12
 * 	1 <= *d <= days_in_month(*y, *m)
 * The input values must be such that the internals don't overflow.
 * The way this routine is used, we don't get close.
 */
static void
normalize_y_m_d(int *y, int *m, int *d)
{
	int dim;	/* # of days in month */

	/* This gets muddy:  the proper range for day can't be determined
	 * without knowing the correct month and year, but if day is, e.g.,
	 * plus or minus a million, the current month and year values make
	 * no sense (and may also be out of bounds themselves).
	 * Saying 12 months == 1 year should be non-controversial.
	 */
	if (*m < 1 || *m > 12) {
		--*m;
		normalize_pair(y, m, 12);
		++*m;
		/* |y| can't be bigger than about
		 * |original y| + |original m|/12 now.
		 */
	}
	assert(1 <= *m && *m <= 12);

	/* Now only day can be out of bounds (year may also be out of bounds
	 * for a datetime object, but we don't care about that here).
	 * If day is out of bounds, what to do is arguable, but at least the
	 * method here is principled and explainable.
	 */
	dim = days_in_month(*y, *m);
	if (*d < 1 || *d > dim) {
		/* Move day-1 days from the first of the month.  First try to
		 * get off cheap if we're only one day out of range
		 * (adjustments for timezone alone can't be worse than that).
		 */
		if (*d == 0) {
			--*m;
			if (*m > 0)
				*d = days_in_month(*y, *m);
			else {
				--*y;
				*m = 12;
				*d = 31;
			}
		}
		else if (*d == dim + 1) {
			/* move forward a day */
			++*m;
			*d = 1;
			if (*m > 12) {
				*m = 1;
				++*y;
			}
		}
		else {
			int ordinal = ymd_to_ord(*y, *m, 1) +
						  *d - 1;
			ord_to_ymd(ordinal, y, m, d);
		}
	}
	assert(*m > 0);
	assert(*d > 0);
}

/* Fiddle out-of-bounds months and days so that the result makes some kind
 * of sense.  The parameters are both inputs and outputs.  Returns < 0 on
 * failure, where failure means the adjusted year is out of bounds.
 */
static int
normalize_date(int *year, int *month, int *day)
{
	int result;

	normalize_y_m_d(year, month, day);
	if (MINYEAR <= *year && *year <= MAXYEAR)
		result = 0;
	else {
		PyErr_SetString(PyExc_OverflowError,
				"date value out of range");
		result = -1;
	}
	return result;
}

/* Force all the datetime fields into range.  The parameters are both
 * inputs and outputs.  Returns < 0 on error.
 */
static int
normalize_datetime(int *year, int *month, int *day,
                   int *hour, int *minute, int *second,
                   int *microsecond)
{
	normalize_pair(second, microsecond, 1000000);
	normalize_pair(minute, second, 60);
	normalize_pair(hour, minute, 60);
	normalize_pair(day, hour, 24);
	return normalize_date(year, month, day);
}


/* ---------------------------------------------------------------------------
 * Helpers for setting object fields.  These work on pointers to the
 * appropriate base class.
 */

/* For date and datetime. */
static void
set_date_fields(PyDateTime_Date *self, int y, int m, int d)
{
	self->hashcode = -1;
	SET_YEAR(self, y);
	SET_MONTH(self, m);
	SET_DAY(self, d);
}

/* ---------------------------------------------------------------------------
 * Create various objects, mostly without range checking.
 */

/* Create a date instance with no range checking. */
static PyObject *
new_date_ex(int year, int month, int day, PyTypeObject *type)
{
	PyDateTime_Date *self;

	self = PyObject_New(type);
	if (self != NULL)
		set_date_fields(self, year, month, day);
	return (PyObject *) self;
}

#define new_date(year, month, day) \
	new_date_ex(year, month, day, &PyDateTime_DateType)

/* Create a datetime instance with no range checking. */
static PyObject *
new_datetime_ex(int year, int month, int day, int hour, int minute,
	     int second, int usecond, PyObject *tzinfo, PyTypeObject *type)
{
	PyDateTime_DateTime *self;
	char aware = tzinfo != Py_None;

	self = PyObject_New(type);
	if (self != NULL) {
		self->hastzinfo = aware;
		set_date_fields((PyDateTime_Date *)self, year, month, day);
		DATE_SET_HOUR(self, hour);
		DATE_SET_MINUTE(self, minute);
		DATE_SET_SECOND(self, second);
		DATE_SET_MICROSECOND(self, usecond);
		if (aware) {
			Py_INCREF(tzinfo);
			self->tzinfo = tzinfo;
		}
	}
	return (PyObject *)self;
}

#define new_datetime(y, m, d, hh, mm, ss, us, tzinfo)		\
	new_datetime_ex(y, m, d, hh, mm, ss, us, tzinfo,	\
			&PyDateTime_DateTimeType)

/* Create a time instance with no range checking. */
static PyObject *
new_time_ex(int hour, int minute, int second, int usecond,
	    PyObject *tzinfo, PyTypeObject *type)
{
	PyDateTime_Time *self;
	char aware = tzinfo != Py_None;

	self = PyObject_New(type);
	if (self != NULL) {
		self->hastzinfo = aware;
		self->hashcode = -1;
		TIME_SET_HOUR(self, hour);
		TIME_SET_MINUTE(self, minute);
		TIME_SET_SECOND(self, second);
		TIME_SET_MICROSECOND(self, usecond);
		if (aware) {
			Py_INCREF(tzinfo);
			self->tzinfo = tzinfo;
		}
	}
	return (PyObject *)self;
}

#define new_time(hh, mm, ss, us, tzinfo)		\
	new_time_ex(hh, mm, ss, us, tzinfo, &PyDateTime_TimeType)

/* Create a timedelta instance.  Normalize the members iff normalize is
 * true.  Passing false is a speed optimization, if you know for sure
 * that seconds and microseconds are already in their proper ranges.  In any
 * case, raises OverflowError and returns NULL if the normalized days is out
 * of range).
 */
static PyObject *
new_delta_ex(int days, int seconds, int microseconds, int normalize,
	     PyTypeObject *type)
{
	PyDateTime_Delta *self;

	if (normalize)
		normalize_d_s_us(&days, &seconds, &microseconds);
	assert(0 <= seconds && seconds < 24*3600);
	assert(0 <= microseconds && microseconds < 1000000);

 	if (check_delta_day_range(days) < 0)
 		return NULL;

	self = PyObject_New(type);
	if (self != NULL) {
		self->hashcode = -1;
		SET_TD_DAYS(self, days);
		SET_TD_SECONDS(self, seconds);
		SET_TD_MICROSECONDS(self, microseconds);
	}
	return (PyObject *) self;
}

#define new_delta(d, s, us, normalize)	\
	new_delta_ex(d, s, us, normalize, &PyDateTime_DeltaType)

/* ---------------------------------------------------------------------------
 * tzinfo helpers.
 */

/* Ensure that p is None or of a tzinfo subclass.  Return 0 if OK; if not
 * raise TypeError and return -1.
 */
static int
check_tzinfo_subclass(PyObject *p)
{
	if (p == Py_None || PyTZInfo_Check(p))
		return 0;
	PyErr_Format(PyExc_TypeError,
		     "tzinfo argument must be None or of a tzinfo subclass, "
		     "not type '%s'",
		     Py_TYPE(p)->tp_name);
	return -1;
}

/* Return tzinfo.methname(tzinfoarg), without any checking of results.
 * If tzinfo is None, returns None.
 */
static PyObject *
call_tzinfo_method(PyObject *tzinfo, char *methname, PyObject *tzinfoarg)
{
	PyObject *result;

	assert(tzinfo && methname && tzinfoarg);
	assert(check_tzinfo_subclass(tzinfo) >= 0);
	if (tzinfo == Py_None) {
		result = Py_None;
		Py_INCREF(result);
	}
	else
		result = PyObject_CallMethod(tzinfo, methname, "O", tzinfoarg);
	return result;
}

/* If self has a tzinfo member, return a BORROWED reference to it.  Else
 * return NULL, which is NOT AN ERROR.  There are no error returns here,
 * and the caller must not decref the result.
 */
static PyObject *
get_tzinfo_member(PyObject *self)
{
	PyObject *tzinfo = NULL;

	if (PyDateTime_Check(self) && HASTZINFO(self))
		tzinfo = ((PyDateTime_DateTime *)self)->tzinfo;
	else if (PyTime_Check(self) && HASTZINFO(self))
		tzinfo = ((PyDateTime_Time *)self)->tzinfo;

	return tzinfo;
}

/* Call getattr(tzinfo, name)(tzinfoarg), and extract an int from the
 * result.  tzinfo must be an instance of the tzinfo class.  If the method
 * returns None, this returns 0 and sets *none to 1.  If the method doesn't
 * return None or timedelta, TypeError is raised and this returns -1.  If it
 * returnsa timedelta and the value is out of range or isn't a whole number
 * of minutes, ValueError is raised and this returns -1.
 * Else *none is set to 0 and the integer method result is returned.
 */
static int
call_utc_tzinfo_method(PyObject *tzinfo, char *name, PyObject *tzinfoarg,
		       int *none)
{
	PyObject *u;
	int result = -1;

	assert(tzinfo != NULL);
	assert(PyTZInfo_Check(tzinfo));
	assert(tzinfoarg != NULL);

	*none = 0;
	u = call_tzinfo_method(tzinfo, name, tzinfoarg);
	if (u == NULL)
		return -1;

	else if (u == Py_None) {
		result = 0;
		*none = 1;
	}
	else if (PyDelta_Check(u)) {
		const int days = GET_TD_DAYS(u);
		if (days < -1 || days > 0)
			result = 24*60;	/* trigger ValueError below */
		else {
			/* next line can't overflow because we know days
			 * is -1 or 0 now
			 */
			int ss = days * 24 * 3600 + GET_TD_SECONDS(u);
			result = divmod(ss, 60, &ss);
			if (ss || GET_TD_MICROSECONDS(u)) {
				PyErr_Format(PyExc_ValueError,
					     "tzinfo.%s() must return a "
					     "whole number of minutes",
					     name);
				result = -1;
			}
		}
	}
	else {
		PyErr_Format(PyExc_TypeError,
			     "tzinfo.%s() must return None or "
			     "timedelta, not '%s'",
			     name, Py_TYPE(u)->tp_name);
	}

	Py_DECREF(u);
	if (result < -1439 || result > 1439) {
		PyErr_Format(PyExc_ValueError,
			     "tzinfo.%s() returned %d; must be in "
			     "-1439 .. 1439",
			     name, result);
		result = -1;
	}
	return result;
}

/* Call tzinfo.utcoffset(tzinfoarg), and extract an integer from the
 * result.  tzinfo must be an instance of the tzinfo class.  If utcoffset()
 * returns None, call_utcoffset returns 0 and sets *none to 1.  If uctoffset()
 * doesn't return None or timedelta, TypeError is raised and this returns -1.
 * If utcoffset() returns an invalid timedelta (out of range, or not a whole
 * # of minutes), ValueError is raised and this returns -1.  Else *none is
 * set to 0 and the offset is returned (as int # of minutes east of UTC).
 */
static int
call_utcoffset(PyObject *tzinfo, PyObject *tzinfoarg, int *none)
{
	return call_utc_tzinfo_method(tzinfo, "utcoffset", tzinfoarg, none);
}

/* Call tzinfo.name(tzinfoarg), and return the offset as a timedelta or None.
 */
static PyObject *
offset_as_timedelta(PyObject *tzinfo, char *name, PyObject *tzinfoarg) {
	PyObject *result;

	assert(tzinfo && name && tzinfoarg);
	if (tzinfo == Py_None) {
		result = Py_None;
		Py_INCREF(result);
	}
	else {
		int none;
		int offset = call_utc_tzinfo_method(tzinfo, name, tzinfoarg,
						    &none);
		if (offset < 0 && PyErr_Occurred())
			return NULL;
		if (none) {
			result = Py_None;
			Py_INCREF(result);
		}
		else
			result = new_delta(0, offset * 60, 0, 1);
	}
	return result;
}

/* Call tzinfo.dst(tzinfoarg), and extract an integer from the
 * result.  tzinfo must be an instance of the tzinfo class.  If dst()
 * returns None, call_dst returns 0 and sets *none to 1.  If dst()
 & doesn't return None or timedelta, TypeError is raised and this
 * returns -1.  If dst() returns an invalid timedelta for a UTC offset,
 * ValueError is raised and this returns -1.  Else *none is set to 0 and
 * the offset is returned (as an int # of minutes east of UTC).
 */
static int
call_dst(PyObject *tzinfo, PyObject *tzinfoarg, int *none)
{
	return call_utc_tzinfo_method(tzinfo, "dst", tzinfoarg, none);
}

/* Call tzinfo.tzname(tzinfoarg), and return the result.  tzinfo must be
 * an instance of the tzinfo class or None.  If tzinfo isn't None, and
 * tzname() doesn't return None or a string, TypeError is raised and this
 * returns NULL.  If the result is a string, we ensure it is a Unicode
 * string.
 */
static PyObject *
call_tzname(PyObject *tzinfo, PyObject *tzinfoarg)
{
	PyObject *result;

	assert(tzinfo != NULL);
	assert(check_tzinfo_subclass(tzinfo) >= 0);
	assert(tzinfoarg != NULL);

	if (tzinfo == Py_None) {
		result = Py_None;
		Py_INCREF(result);
	}
	else
		result = PyObject_CallMethod(tzinfo, "tzname", "O", tzinfoarg);

	if (result != NULL && result != Py_None) {
		if (!PyUnicode_Check(result)) {
			PyErr_Format(PyExc_TypeError, "tzinfo.tzname() must "
				     "return None or a string, not '%s'",
				     Py_TYPE(result)->tp_name);
			Py_DECREF(result);
			result = NULL;
		}
		else if (!PyUnicode_Check(result)) {
			PyObject *temp = PyUnicode_FromObject(result);
			Py_DECREF(result);
			result = temp;
		}
	}
	return result;
}

typedef enum {
	      /* an exception has been set; the caller should pass it on */
	      OFFSET_ERROR,

	      /* type isn't date, datetime, or time subclass */
	      OFFSET_UNKNOWN,

	      /* date,
	       * datetime with !hastzinfo
	       * datetime with None tzinfo,
	       * datetime where utcoffset() returns None
	       * time with !hastzinfo
	       * time with None tzinfo,
	       * time where utcoffset() returns None
	       */
	      OFFSET_NAIVE,

	      /* time or datetime where utcoffset() doesn't return None */
	      OFFSET_AWARE
} naivety;

/* Classify an object as to whether it's naive or offset-aware.  See
 * the "naivety" typedef for details.  If the type is aware, *offset is set
 * to minutes east of UTC (as returned by the tzinfo.utcoffset() method).
 * If the type is offset-naive (or unknown, or error), *offset is set to 0.
 * tzinfoarg is the argument to pass to the tzinfo.utcoffset() method.
 */
static naivety
classify_utcoffset(PyObject *op, PyObject *tzinfoarg, int *offset)
{
	int none;
	PyObject *tzinfo;

	assert(tzinfoarg != NULL);
	*offset = 0;
	tzinfo = get_tzinfo_member(op);	/* NULL means no tzinfo, not error */
	if (tzinfo == Py_None)
		return OFFSET_NAIVE;
	if (tzinfo == NULL) {
		/* note that a datetime passes the PyDate_Check test */
		return (PyTime_Check(op) || PyDate_Check(op)) ?
		       OFFSET_NAIVE : OFFSET_UNKNOWN;
	}
	*offset = call_utcoffset(tzinfo, tzinfoarg, &none);
	if (*offset == -1 && PyErr_Occurred())
		return OFFSET_ERROR;
	return none ? OFFSET_NAIVE : OFFSET_AWARE;
}

/* Classify two objects as to whether they're naive or offset-aware.
 * This isn't quite the same as calling classify_utcoffset() twice:  for
 * binary operations (comparison and subtraction), we generally want to
 * ignore the tzinfo members if they're identical.  This is by design,
 * so that results match "naive" expectations when mixing objects from a
 * single timezone.  So in that case, this sets both offsets to 0 and
 * both naiveties to OFFSET_NAIVE.
 * The function returns 0 if everything's OK, and -1 on error.
 */
static int
classify_two_utcoffsets(PyObject *o1, int *offset1, naivety *n1,
			PyObject *tzinfoarg1,
			PyObject *o2, int *offset2, naivety *n2,
			PyObject *tzinfoarg2)
{
	if (get_tzinfo_member(o1) == get_tzinfo_member(o2)) {
		*offset1 = *offset2 = 0;
		*n1 = *n2 = OFFSET_NAIVE;
	}
	else {
		*n1 = classify_utcoffset(o1, tzinfoarg1, offset1);
		if (*n1 == OFFSET_ERROR)
			return -1;
		*n2 = classify_utcoffset(o2, tzinfoarg2, offset2);
		if (*n2 == OFFSET_ERROR)
			return -1;
	}
	return 0;
}

/* repr is like "someclass(arg1, arg2)".  If tzinfo isn't None,
 * stuff
 *     ", tzinfo=" + repr(tzinfo)
 * before the closing ")".
 */
static PyObject *
append_keyword_tzinfo(PyObject *repr, PyObject *tzinfo)
{
	PyObject *temp;

	assert(PyUnicode_Check(repr));
	assert(tzinfo);
	if (tzinfo == Py_None)
		return repr;
	/* Get rid of the trailing ')'. */
	assert(PyUnicode_AS_UNICODE(repr)[PyUnicode_GET_SIZE(repr)-1] == ')');
	temp = PyUnicode_FromUnicode(PyUnicode_AS_UNICODE(repr),
					  PyUnicode_GET_SIZE(repr) - 1);
	Py_DECREF(repr);
	if (temp == NULL)
		return NULL;
	repr = PyUnicode_FromFormat("%U, tzinfo=%R)", temp, tzinfo);
	Py_DECREF(temp);
	return repr;
}

/* ---------------------------------------------------------------------------
 * String format helpers.
 */

static PyObject *
format_ctime(PyDateTime_Date *date, int hours, int minutes, int seconds)
{
	static const char *DayNames[] = {
		"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
	};
	static const char *MonthNames[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};

	int wday = weekday(GET_YEAR(date), GET_MONTH(date), GET_DAY(date));

	return PyUnicode_FromFormat("%s %s %2d %02d:%02d:%02d %04d",
	                            DayNames[wday], MonthNames[GET_MONTH(date)-1],
	                            GET_DAY(date), hours, minutes, seconds,
	                            GET_YEAR(date));
}

/* Add an hours & minutes UTC offset string to buf.  buf has no more than
 * buflen bytes remaining.  The UTC offset is gotten by calling
 * tzinfo.uctoffset(tzinfoarg).  If that returns None, \0 is stored into
 * *buf, and that's all.  Else the returned value is checked for sanity (an
 * integer in range), and if that's OK it's converted to an hours & minutes
 * string of the form
 *   sign HH sep MM
 * Returns 0 if everything is OK.  If the return value from utcoffset() is
 * bogus, an appropriate exception is set and -1 is returned.
 */
static int
format_utcoffset(char *buf, size_t buflen, const char *sep,
		PyObject *tzinfo, PyObject *tzinfoarg)
{
	int offset;
	int hours;
	int minutes;
	char sign;
	int none;

	offset = call_utcoffset(tzinfo, tzinfoarg, &none);
	if (offset == -1 && PyErr_Occurred())
		return -1;
	if (none) {
		*buf = '\0';
		return 0;
	}
	sign = '+';
	if (offset < 0) {
		sign = '-';
		offset = - offset;
	}
	hours = divmod(offset, 60, &minutes);
	PyOS_snprintf(buf, buflen, "%c%02d%s%02d", sign, hours, sep, minutes);
	return 0;
}

static PyObject *
make_Zreplacement(PyObject *object, PyObject *tzinfoarg)
{
	PyObject *temp;
	PyObject *tzinfo = get_tzinfo_member(object);
	PyObject *Zreplacement = PyUnicode_FromStringAndSize(NULL, 0);
	if (Zreplacement == NULL)
		return NULL;
	if (tzinfo == Py_None || tzinfo == NULL)
		return Zreplacement;

	assert(tzinfoarg != NULL);
	temp = call_tzname(tzinfo, tzinfoarg);
	if (temp == NULL)
		goto Error;
	if (temp == Py_None) {
		Py_DECREF(temp);
		return Zreplacement;
	}

	assert(PyUnicode_Check(temp));
	/* Since the tzname is getting stuffed into the
	 * format, we have to double any % signs so that
	 * strftime doesn't treat them as format codes.
	 */
	Py_DECREF(Zreplacement);
	Zreplacement = PyObject_CallMethod(temp, "replace", "ss", "%", "%%");
	Py_DECREF(temp);
	if (Zreplacement == NULL)
		return NULL;
	if (!PyUnicode_Check(Zreplacement)) {
		PyErr_SetString(PyExc_TypeError,
				"tzname.replace() did not return a string");
		goto Error;
	}
	return Zreplacement;

  Error:
	Py_DECREF(Zreplacement);
	return NULL;
}

static PyObject *
make_freplacement(PyObject *object)
{
	char freplacement[64];
	if (PyTime_Check(object))
	    sprintf(freplacement, "%06d", TIME_GET_MICROSECOND(object));
	else if (PyDateTime_Check(object))
	    sprintf(freplacement, "%06d", DATE_GET_MICROSECOND(object));
	else
	    sprintf(freplacement, "%06d", 0);

	return PyString_FromStringAndSize(freplacement, strlen(freplacement));
}

/* I sure don't want to reproduce the strftime code from the time module,
 * so this imports the module and calls it.  All the hair is due to
 * giving special meanings to the %z, %Z and %f format codes via a
 * preprocessing step on the format string.
 * tzinfoarg is the argument to pass to the object's tzinfo method, if
 * needed.
 */
static PyObject *
wrap_strftime(PyObject *object, PyObject *format, PyObject *timetuple,
	      PyObject *tzinfoarg)
{
	PyObject *result = NULL;	/* guilty until proved innocent */

	PyObject *zreplacement = NULL;	/* py string, replacement for %z */
	PyObject *Zreplacement = NULL;	/* py string, replacement for %Z */
	PyObject *freplacement = NULL;	/* py string, replacement for %f */

	const char *pin;/* pointer to next char in input format */
        Py_ssize_t flen;/* length of input format */
	char ch;	/* next char in input format */

	PyObject *newfmt = NULL;	/* py string, the output format */
	char *pnew;	/* pointer to available byte in output format */
	int totalnew;	/* number bytes total in output format buffer,
			   exclusive of trailing \0 */
	int usednew;	/* number bytes used so far in output format buffer */

	const char *ptoappend;/* pointer to string to append to output buffer */
	Py_ssize_t ntoappend;	/* # of bytes to append to output buffer */

	assert(object && format && timetuple);
	assert(PyUnicode_Check(format));
	/* Convert the input format to a C string and size */
	pin = PyUnicode_AsString(format);
	if (!pin)
		return NULL;
	flen = PyUnicode_GetSize(format);

	/* Give up if the year is before 1900.
	 * Python strftime() plays games with the year, and different
	 * games depending on whether envar PYTHON2K is set.  This makes
	 * years before 1900 a nightmare, even if the platform strftime
	 * supports them (and not all do).
	 * We could get a lot farther here by avoiding Python's strftime
	 * wrapper and calling the C strftime() directly, but that isn't
	 * an option in the Python implementation of this module.
	 */
	{
		long year;
		PyObject *pyyear = PySequence_GetItem(timetuple, 0);
		if (pyyear == NULL) return NULL;
		assert(PyLong_Check(pyyear));
		year = PyLong_AsLong(pyyear);
		Py_DECREF(pyyear);
		if (year < 1900) {
			PyErr_Format(PyExc_ValueError, "year=%ld is before "
				     "1900; the datetime strftime() "
	                             "methods require year >= 1900",
	                             year);
	                return NULL;
		}
	}

	/* Scan the input format, looking for %z/%Z/%f escapes, building
	 * a new format.  Since computing the replacements for those codes
	 * is expensive, don't unless they're actually used.
	 */
	totalnew = flen + 1;	/* realistic if no %z/%Z */
	newfmt = PyString_FromStringAndSize(NULL, totalnew);
	if (newfmt == NULL) goto Done;
	pnew = PyString_AsString(newfmt);
	usednew = 0;

	while ((ch = *pin++) != '\0') {
		if (ch != '%') {
			ptoappend = pin - 1;
			ntoappend = 1;
		}
		else if ((ch = *pin++) == '\0') {
			/* There's a lone trailing %; doesn't make sense. */
			PyErr_SetString(PyExc_ValueError, "strftime format "
					"ends with raw %");
			goto Done;
		}
		/* A % has been seen and ch is the character after it. */
		else if (ch == 'z') {
			if (zreplacement == NULL) {
				/* format utcoffset */
				char buf[100];
				PyObject *tzinfo = get_tzinfo_member(object);
				zreplacement = PyString_FromStringAndSize("", 0);
				if (zreplacement == NULL) goto Done;
				if (tzinfo != Py_None && tzinfo != NULL) {
					assert(tzinfoarg != NULL);
					if (format_utcoffset(buf,
							     sizeof(buf),
							     "",
							     tzinfo,
							     tzinfoarg) < 0)
						goto Done;
					Py_DECREF(zreplacement);
					zreplacement =
					  PyString_FromStringAndSize(buf,
								   strlen(buf));
					if (zreplacement == NULL)
						goto Done;
				}
			}
			assert(zreplacement != NULL);
			ptoappend = PyString_AS_STRING(zreplacement);
			ntoappend = PyString_GET_SIZE(zreplacement);
		}
		else if (ch == 'Z') {
			/* format tzname */
			if (Zreplacement == NULL) {
				Zreplacement = make_Zreplacement(object,
								 tzinfoarg);
				if (Zreplacement == NULL)
					goto Done;
			}
			assert(Zreplacement != NULL);
			assert(PyUnicode_Check(Zreplacement));
			ptoappend = PyUnicode_AsStringAndSize(Zreplacement,
                                                              &ntoappend);
			ntoappend = Py_SIZE(Zreplacement);
		}
		else if (ch == 'f') {
			/* format microseconds */
			if (freplacement == NULL) {
				freplacement = make_freplacement(object);
				if (freplacement == NULL)
					goto Done;
			}
			assert(freplacement != NULL);
			assert(PyString_Check(freplacement));
			ptoappend = PyString_AS_STRING(freplacement);
			ntoappend = PyString_GET_SIZE(freplacement);
		}
		else {
			/* percent followed by neither z nor Z */
			ptoappend = pin - 2;
			ntoappend = 2;
		}

 		/* Append the ntoappend chars starting at ptoappend to
 		 * the new format.
 		 */
 		if (ntoappend == 0)
 			continue;
 		assert(ptoappend != NULL);
 		assert(ntoappend > 0);
 		while (usednew + ntoappend > totalnew) {
 			int bigger = totalnew << 1;
 			if ((bigger >> 1) != totalnew) { /* overflow */
 				PyErr_NoMemory();
 				goto Done;
 			}
 			if (_PyString_Resize(&newfmt, bigger) < 0)
 				goto Done;
 			totalnew = bigger;
 			pnew = PyString_AsString(newfmt) + usednew;
 		}
		memcpy(pnew, ptoappend, ntoappend);
		pnew += ntoappend;
		usednew += ntoappend;
		assert(usednew <= totalnew);
	}  /* end while() */

	if (_PyString_Resize(&newfmt, usednew) < 0)
		goto Done;
	{
		PyObject *format;
		PyObject *time = PyImport_ImportModuleNoBlock("time");
		if (time == NULL)
			goto Done;
		format = PyUnicode_FromString(PyString_AS_STRING(newfmt));
		if (format != NULL) {
			result = PyObject_CallMethod(time, "strftime", "OO",
						     format, timetuple);
			Py_DECREF(format);
		}
		Py_DECREF(time);
    	}
 Done:
	Py_XDECREF(freplacement);
	Py_XDECREF(zreplacement);
	Py_XDECREF(Zreplacement);
	Py_XDECREF(newfmt);
    	return result;
}

/* ---------------------------------------------------------------------------
 * Wrap functions from the time module.  These aren't directly available
 * from C.  Perhaps they should be.
 */

/* Call time.time() and return its result (a Python float). */
static PyObject *
time_time(void)
{
	PyObject *result = NULL;
	PyObject *time = PyImport_ImportModuleNoBlock("time");

	if (time != NULL) {
		result = PyObject_CallMethod(time, "time", "()");
		Py_DECREF(time);
	}
	return result;
}

/* Build a time.struct_time.  The weekday and day number are automatically
 * computed from the y,m,d args.
 */
static PyObject *
build_struct_time(int y, int m, int d, int hh, int mm, int ss, int dstflag)
{
	PyObject *time;
	PyObject *result = NULL;

	time = PyImport_ImportModuleNoBlock("time");
	if (time != NULL) {
		result = PyObject_CallMethod(time, "struct_time",
					     "((iiiiiiiii))",
					     y, m, d,
					     hh, mm, ss,
				 	     weekday(y, m, d),
				 	     days_before_month(y, m) + d,
				 	     dstflag);
		Py_DECREF(time);
	}
	return result;
}

/* ---------------------------------------------------------------------------
 * Miscellaneous helpers.
 */

/* For various reasons, we need to use tp_richcompare instead of tp_compare.
 * The comparisons here all most naturally compute a cmp()-like result.
 * This little helper turns that into a bool result for rich comparisons.
 */
static PyObject *
diff_to_bool(int diff, int op)
{
	PyObject *result;
	int istrue;

	switch (op) {
		case Py_EQ: istrue = diff == 0; break;
		case Py_NE: istrue = diff != 0; break;
		case Py_LE: istrue = diff <= 0; break;
		case Py_GE: istrue = diff >= 0; break;
		case Py_LT: istrue = diff < 0; break;
		case Py_GT: istrue = diff > 0; break;
		default:
			assert(! "op unknown");
			istrue = 0; /* To shut up compiler */
	}
	result = istrue ? Py_True : Py_False;
	Py_INCREF(result);
	return result;
}

/* Raises a "can't compare" TypeError and returns NULL. */
static PyObject *
cmperror(PyObject *a, PyObject *b)
{
	PyErr_Format(PyExc_TypeError,
		     "can't compare %s to %s",
		     Py_TYPE(a)->tp_name, Py_TYPE(b)->tp_name);
	return NULL;
}

/* ---------------------------------------------------------------------------
 * Cached Python objects; these are set by the module init function.
 */

/* Conversion factors. */
static PyObject *us_per_us = NULL;	/* 1 */
static PyObject *us_per_ms = NULL;	/* 1000 */
static PyObject *us_per_second = NULL;	/* 1000000 */
static PyObject *us_per_minute = NULL;	/* 1e6 * 60 as Python int */
static PyObject *us_per_hour = NULL;	/* 1e6 * 3600 as Python long */
static PyObject *us_per_day = NULL;	/* 1e6 * 3600 * 24 as Python long */
static PyObject *us_per_week = NULL;	/* 1e6*3600*24*7 as Python long */
static PyObject *seconds_per_day = NULL; /* 3600*24 as Python int */

/* ---------------------------------------------------------------------------
 * Class implementations.
 */

/*
 * PyDateTime_Delta implementation.
 */

/* Convert a timedelta to a number of us,
 * 	(24*3600*self.days + self.seconds)*1000000 + self.microseconds
 * as a Python int or long.
 * Doing mixed-radix arithmetic by hand instead is excruciating in C,
 * due to ubiquitous overflow possibilities.
 */
static PyObject *
delta_to_microseconds(PyDateTime_Delta *self)
{
	PyObject *x1 = NULL;
	PyObject *x2 = NULL;
	PyObject *x3 = NULL;
	PyObject *result = NULL;

	x1 = PyLong_FromLong(GET_TD_DAYS(self));
	if (x1 == NULL)
		goto Done;
	x2 = PyNumber_Multiply(x1, seconds_per_day);	/* days in seconds */
	if (x2 == NULL)
		goto Done;
	Py_DECREF(x1);
	x1 = NULL;

	/* x2 has days in seconds */
	x1 = PyLong_FromLong(GET_TD_SECONDS(self));	/* seconds */
	if (x1 == NULL)
		goto Done;
	x3 = PyNumber_Add(x1, x2);	/* days and seconds in seconds */
	if (x3 == NULL)
		goto Done;
	Py_DECREF(x1);
	Py_DECREF(x2);
	x1 = x2 = NULL;

	/* x3 has days+seconds in seconds */
	x1 = PyNumber_Multiply(x3, us_per_second);	/* us */
	if (x1 == NULL)
		goto Done;
	Py_DECREF(x3);
	x3 = NULL;

	/* x1 has days+seconds in us */
	x2 = PyLong_FromLong(GET_TD_MICROSECONDS(self));
	if (x2 == NULL)
		goto Done;
	result = PyNumber_Add(x1, x2);

Done:
	Py_XDECREF(x1);
	Py_XDECREF(x2);
	Py_XDECREF(x3);
	return result;
}

/* Convert a number of us (as a Python int or long) to a timedelta.
 */
static PyObject *
microseconds_to_delta_ex(PyObject *pyus, PyTypeObject *type)
{
	int us;
	int s;
	int d;
	long temp;

	PyObject *tuple = NULL;
	PyObject *num = NULL;
	PyObject *result = NULL;

	tuple = PyNumber_Divmod(pyus, us_per_second);
	if (tuple == NULL)
		goto Done;

	num = PyTuple_GetItem(tuple, 1);	/* us */
	if (num == NULL)
		goto Done;
	temp = PyLong_AsLong(num);
	num = NULL;
	if (temp == -1 && PyErr_Occurred())
		goto Done;
	assert(0 <= temp && temp < 1000000);
	us = (int)temp;
	if (us < 0) {
		/* The divisor was positive, so this must be an error. */
		assert(PyErr_Occurred());
		goto Done;
	}

	num = PyTuple_GetItem(tuple, 0);	/* leftover seconds */
	if (num == NULL)
		goto Done;
	Py_INCREF(num);
	Py_DECREF(tuple);

	tuple = PyNumber_Divmod(num, seconds_per_day);
	if (tuple == NULL)
		goto Done;
	Py_DECREF(num);

	num = PyTuple_GetItem(tuple, 1); 	/* seconds */
	if (num == NULL)
		goto Done;
	temp = PyLong_AsLong(num);
	num = NULL;
	if (temp == -1 && PyErr_Occurred())
		goto Done;
	assert(0 <= temp && temp < 24*3600);
	s = (int)temp;

	if (s < 0) {
		/* The divisor was positive, so this must be an error. */
		assert(PyErr_Occurred());
		goto Done;
	}

	num = PyTuple_GetItem(tuple, 0);	/* leftover days */
	if (num == NULL)
		goto Done;
	Py_INCREF(num);
	temp = PyLong_AsLong(num);
	if (temp == -1 && PyErr_Occurred())
		goto Done;
	d = (int)temp;
	if ((long)d != temp) {
		PyErr_SetString(PyExc_OverflowError, "normalized days too "
				"large to fit in a C int");
		goto Done;
	}
	result = new_delta_ex(d, s, us, 0, type);

Done:
	Py_XDECREF(tuple);
	Py_XDECREF(num);
	return result;
}

#define microseconds_to_delta(pymicros)	\
	microseconds_to_delta_ex(pymicros, &PyDateTime_DeltaType)

static PyObject *
multiply_int_timedelta(PyObject *intobj, PyDateTime_Delta *delta)
{
	PyObject *pyus_in;
	PyObject *pyus_out;
	PyObject *result;

	pyus_in = delta_to_microseconds(delta);
	if (pyus_in == NULL)
		return NULL;

	pyus_out = PyNumber_Multiply(pyus_in, intobj);
	Py_DECREF(pyus_in);
	if (pyus_out == NULL)
		return NULL;

	result = microseconds_to_delta(pyus_out);
	Py_DECREF(pyus_out);
	return result;
}

static PyObject *
divide_timedelta_int(PyDateTime_Delta *delta, PyObject *intobj)
{
	PyObject *pyus_in;
	PyObject *pyus_out;
	PyObject *result;

	pyus_in = delta_to_microseconds(delta);
	if (pyus_in == NULL)
		return NULL;

	pyus_out = PyNumber_FloorDivide(pyus_in, intobj);
	Py_DECREF(pyus_in);
	if (pyus_out == NULL)
		return NULL;

	result = microseconds_to_delta(pyus_out);
	Py_DECREF(pyus_out);
	return result;
}

static PyObject *
delta_add(PyObject *left, PyObject *right)
{
	PyObject *result = Py_NotImplemented;

	if (PyDelta_Check(left) && PyDelta_Check(right)) {
		/* delta + delta */
		/* The C-level additions can't overflow because of the
		 * invariant bounds.
		 */
		int days = GET_TD_DAYS(left) + GET_TD_DAYS(right);
		int seconds = GET_TD_SECONDS(left) + GET_TD_SECONDS(right);
		int microseconds = GET_TD_MICROSECONDS(left) +
				   GET_TD_MICROSECONDS(right);
		result = new_delta(days, seconds, microseconds, 1);
	}

	if (result == Py_NotImplemented)
		Py_INCREF(result);
	return result;
}

static PyObject *
delta_negative(PyDateTime_Delta *self)
{
	return new_delta(-GET_TD_DAYS(self),
			 -GET_TD_SECONDS(self),
			 -GET_TD_MICROSECONDS(self),
			 1);
}

static PyObject *
delta_positive(PyDateTime_Delta *self)
{
	/* Could optimize this (by returning self) if this isn't a
	 * subclass -- but who uses unary + ?  Approximately nobody.
	 */
	return new_delta(GET_TD_DAYS(self),
			 GET_TD_SECONDS(self),
			 GET_TD_MICROSECONDS(self),
			 0);
}

static PyObject *
delta_abs(PyDateTime_Delta *self)
{
	PyObject *result;

	assert(GET_TD_MICROSECONDS(self) >= 0);
	assert(GET_TD_SECONDS(self) >= 0);

	if (GET_TD_DAYS(self) < 0)
		result = delta_negative(self);
	else
		result = delta_positive(self);

	return result;
}

static PyObject *
delta_subtract(PyObject *left, PyObject *right)
{
	PyObject *result = Py_NotImplemented;

	if (PyDelta_Check(left) && PyDelta_Check(right)) {
	    	/* delta - delta */
	    	PyObject *minus_right = PyNumber_Negative(right);
	    	if (minus_right) {
	    		result = delta_add(left, minus_right);
	    		Py_DECREF(minus_right);
	    	}
	    	else
	    		result = NULL;
	}

	if (result == Py_NotImplemented)
		Py_INCREF(result);
	return result;
}

static PyObject *
delta_richcompare(PyObject *self, PyObject *other, int op)
{
	if (PyDelta_Check(other)) {
		int diff = GET_TD_DAYS(self) - GET_TD_DAYS(other);
		if (diff == 0) {
			diff = GET_TD_SECONDS(self) - GET_TD_SECONDS(other);
			if (diff == 0)
				diff = GET_TD_MICROSECONDS(self) -
				       GET_TD_MICROSECONDS(other);
		}
		return diff_to_bool(diff, op);
	}
	else {
		Py_INCREF(Py_NotImplemented);
		return Py_NotImplemented;
	}
}

static PyObject *delta_getstate(PyDateTime_Delta *self);

static long
delta_hash(PyDateTime_Delta *self)
{
	if (self->hashcode == -1) {
		PyObject *temp = delta_getstate(self);
		if (temp != NULL) {
			self->hashcode = PyObject_Hash(temp);
			Py_DECREF(temp);
		}
	}
	return self->hashcode;
}

static PyObject *
delta_multiply(PyObject *left, PyObject *right)
{
	PyObject *result = Py_NotImplemented;

	if (PyDelta_Check(left)) {
		/* delta * ??? */
		if (PyLong_Check(right))
			result = multiply_int_timedelta(right,
					(PyDateTime_Delta *) left);
	}
	else if (PyLong_Check(left))
		result = multiply_int_timedelta(left,
						(PyDateTime_Delta *) right);

	if (result == Py_NotImplemented)
		Py_INCREF(result);
	return result;
}

static PyObject *
delta_divide(PyObject *left, PyObject *right)
{
	PyObject *result = Py_NotImplemented;

	if (PyDelta_Check(left)) {
		/* delta * ??? */
		if (PyLong_Check(right))
			result = divide_timedelta_int(
					(PyDateTime_Delta *)left,
					right);
	}

	if (result == Py_NotImplemented)
		Py_INCREF(result);
	return result;
}

/* Fold in the value of the tag ("seconds", "weeks", etc) component of a
 * timedelta constructor.  sofar is the # of microseconds accounted for
 * so far, and there are factor microseconds per current unit, the number
 * of which is given by num.  num * factor is added to sofar in a
 * numerically careful way, and that's the result.  Any fractional
 * microseconds left over (this can happen if num is a float type) are
 * added into *leftover.
 * Note that there are many ways this can give an error (NULL) return.
 */
static PyObject *
accum(const char* tag, PyObject *sofar, PyObject *num, PyObject *factor,
      double *leftover)
{
	PyObject *prod;
	PyObject *sum;

	assert(num != NULL);

	if (PyLong_Check(num)) {
		prod = PyNumber_Multiply(num, factor);
		if (prod == NULL)
			return NULL;
		sum = PyNumber_Add(sofar, prod);
		Py_DECREF(prod);
		return sum;
	}

	if (PyFloat_Check(num)) {
		double dnum;
		double fracpart;
		double intpart;
		PyObject *x;
		PyObject *y;

		/* The Plan:  decompose num into an integer part and a
		 * fractional part, num = intpart + fracpart.
		 * Then num * factor ==
		 *      intpart * factor + fracpart * factor
		 * and the LHS can be computed exactly in long arithmetic.
		 * The RHS is again broken into an int part and frac part.
		 * and the frac part is added into *leftover.
		 */
		dnum = PyFloat_AsDouble(num);
		if (dnum == -1.0 && PyErr_Occurred())
			return NULL;
		fracpart = modf(dnum, &intpart);
		x = PyLong_FromDouble(intpart);
		if (x == NULL)
			return NULL;

		prod = PyNumber_Multiply(x, factor);
		Py_DECREF(x);
		if (prod == NULL)
			return NULL;

		sum = PyNumber_Add(sofar, prod);
		Py_DECREF(prod);
		if (sum == NULL)
			return NULL;

		if (fracpart == 0.0)
			return sum;
		/* So far we've lost no information.  Dealing with the
		 * fractional part requires float arithmetic, and may
		 * lose a little info.
		 */
		assert(PyLong_Check(factor));
		dnum = PyLong_AsDouble(factor);

		dnum *= fracpart;
		fracpart = modf(dnum, &intpart);
		x = PyLong_FromDouble(intpart);
		if (x == NULL) {
			Py_DECREF(sum);
			return NULL;
		}

		y = PyNumber_Add(sum, x);
		Py_DECREF(sum);
		Py_DECREF(x);
		*leftover += fracpart;
		return y;
	}

	PyErr_Format(PyExc_TypeError,
		     "unsupported type for timedelta %s component: %s",
		     tag, Py_TYPE(num)->tp_name);
	return NULL;
}

static PyObject *
delta_new(PyTypeObject *type, PyObject *args, PyObject *kw)
{
	PyObject *self = NULL;

	/* Argument objects. */
	PyObject *day = NULL;
	PyObject *second = NULL;
	PyObject *us = NULL;
	PyObject *ms = NULL;
	PyObject *minute = NULL;
	PyObject *hour = NULL;
	PyObject *week = NULL;

	PyObject *x = NULL;	/* running sum of microseconds */
	PyObject *y = NULL;	/* temp sum of microseconds */
	double leftover_us = 0.0;

	static char *keywords[] = {
		"days", "seconds", "microseconds", "milliseconds",
		"minutes", "hours", "weeks", NULL
	};

	if (PyArg_ParseTupleAndKeywords(args, kw, "|OOOOOOO:__new__",
					keywords,
					&day, &second, &us,
					&ms, &minute, &hour, &week) == 0)
		goto Done;

	x = PyLong_FromLong(0);
	if (x == NULL)
		goto Done;

#define CLEANUP 	\
	Py_DECREF(x);	\
	x = y;		\
	if (x == NULL)	\
		goto Done

	if (us) {
		y = accum("microseconds", x, us, us_per_us, &leftover_us);
		CLEANUP;
	}
	if (ms) {
		y = accum("milliseconds", x, ms, us_per_ms, &leftover_us);
		CLEANUP;
	}
	if (second) {
		y = accum("seconds", x, second, us_per_second, &leftover_us);
		CLEANUP;
	}
	if (minute) {
		y = accum("minutes", x, minute, us_per_minute, &leftover_us);
		CLEANUP;
	}
	if (hour) {
		y = accum("hours", x, hour, us_per_hour, &leftover_us);
		CLEANUP;
	}
	if (day) {
		y = accum("days", x, day, us_per_day, &leftover_us);
		CLEANUP;
	}
	if (week) {
		y = accum("weeks", x, week, us_per_week, &leftover_us);
		CLEANUP;
	}
	if (leftover_us) {
		/* Round to nearest whole # of us, and add into x. */
		PyObject *temp = PyLong_FromLong(round_to_long(leftover_us));
		if (temp == NULL) {
			Py_DECREF(x);
			goto Done;
		}
		y = PyNumber_Add(x, temp);
		Py_DECREF(temp);
		CLEANUP;
	}

	self = microseconds_to_delta_ex(x, type);
	Py_DECREF(x);
Done:
	return self;

#undef CLEANUP
}

static int
delta_bool(PyDateTime_Delta *self)
{
	return (GET_TD_DAYS(self) != 0
		|| GET_TD_SECONDS(self) != 0
		|| GET_TD_MICROSECONDS(self) != 0);
}

static PyObject *
delta_repr(PyDateTime_Delta *self)
{
	if (GET_TD_MICROSECONDS(self) != 0)
		return PyUnicode_FromFormat("%s(%d, %d, %d)",
		                            Py_TYPE(self)->tp_name,
		                            GET_TD_DAYS(self),
		                            GET_TD_SECONDS(self),
		                            GET_TD_MICROSECONDS(self));
	if (GET_TD_SECONDS(self) != 0)
		return PyUnicode_FromFormat("%s(%d, %d)",
		                            Py_TYPE(self)->tp_name,
		                            GET_TD_DAYS(self),
		                            GET_TD_SECONDS(self));

	return PyUnicode_FromFormat("%s(%d)",
	                            Py_TYPE(self)->tp_name,
	                            GET_TD_DAYS(self));
}

static PyObject *
delta_str(PyDateTime_Delta *self)
{
	int us = GET_TD_MICROSECONDS(self);
	int seconds = GET_TD_SECONDS(self);
	int minutes = divmod(seconds, 60, &seconds);
	int hours = divmod(minutes, 60, &minutes);
	int days = GET_TD_DAYS(self);

	if (days) {
		if (us)
			return PyUnicode_FromFormat("%d day%s, %d:%02d:%02d.%06d",
			                            days, (days == 1 || days == -1) ? "" : "s",
			                            hours, minutes, seconds, us);
		else
			return PyUnicode_FromFormat("%d day%s, %d:%02d:%02d",
			                            days, (days == 1 || days == -1) ? "" : "s",
			                            hours, minutes, seconds);
	} else {
		if (us)
			return PyUnicode_FromFormat("%d:%02d:%02d.%06d",
			                            hours, minutes, seconds, us);
		else
			return PyUnicode_FromFormat("%d:%02d:%02d",
			                            hours, minutes, seconds);
	}

}

/* Pickle support, a simple use of __reduce__. */

/* __getstate__ isn't exposed */
static PyObject *
delta_getstate(PyDateTime_Delta *self)
{
	return Py_BuildValue("iii", GET_TD_DAYS(self),
				    GET_TD_SECONDS(self),
				    GET_TD_MICROSECONDS(self));
}

static PyObject *
delta_reduce(PyDateTime_Delta* self)
{
	return Py_BuildValue("ON", Py_TYPE(self), delta_getstate(self));
}

#define OFFSET(field)  offsetof(PyDateTime_Delta, field)

static PyMemberDef delta_members[] = {

	{"days",         T_INT, OFFSET(days),         READONLY,
	 PyDoc_STR("Number of days.")},

	{"seconds",      T_INT, OFFSET(seconds),      READONLY,
	 PyDoc_STR("Number of seconds (>= 0 and less than 1 day).")},

	{"microseconds", T_INT, OFFSET(microseconds), READONLY,
	 PyDoc_STR("Number of microseconds (>= 0 and less than 1 second).")},
	{NULL}
};

static PyMethodDef delta_methods[] = {
	{"__reduce__", (PyCFunction)delta_reduce,     METH_NOARGS,
	 PyDoc_STR("__reduce__() -> (cls, state)")},

	{NULL,	NULL},
};

static char delta_doc[] =
PyDoc_STR("Difference between two datetime values.");

static PyNumberMethods delta_as_number = {
	delta_add,				/* nb_add */
	delta_subtract,				/* nb_subtract */
	delta_multiply,				/* nb_multiply */
	0,					/* nb_remainder */
	0,					/* nb_divmod */
	0,					/* nb_power */
	(unaryfunc)delta_negative,		/* nb_negative */
	(unaryfunc)delta_positive,		/* nb_positive */
	(unaryfunc)delta_abs,			/* nb_absolute */
	(inquiry)delta_bool,			/* nb_bool */
	0,					/*nb_invert*/
	0,					/*nb_lshift*/
	0,					/*nb_rshift*/
	0,					/*nb_and*/
	0,					/*nb_xor*/
	0,					/*nb_or*/
	0,					/*nb_reserved*/
	0,					/*nb_int*/
	0,					/*nb_long*/
	0,					/*nb_float*/
	0,					/*nb_oct*/
	0, 					/*nb_hex*/
	0,					/*nb_inplace_add*/
	0,					/*nb_inplace_subtract*/
	0,					/*nb_inplace_multiply*/
	0,					/*nb_inplace_remainder*/
	0,					/*nb_inplace_power*/
	0,					/*nb_inplace_lshift*/
	0,					/*nb_inplace_rshift*/
	0,					/*nb_inplace_and*/
	0,					/*nb_inplace_xor*/
	0,					/*nb_inplace_or*/
	delta_divide,				/* nb_floor_divide */
	0,					/* nb_true_divide */
	0,					/* nb_inplace_floor_divide */
	0,					/* nb_inplace_true_divide */
};

static PyTypeObject PyDateTime_DeltaType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"datetime.timedelta",				/* tp_name */
	sizeof(PyDateTime_Delta),			/* tp_basicsize */
	0,						/* tp_itemsize */
	0,						/* tp_dealloc */
	0,						/* tp_print */
	0,						/* tp_getattr */
	0,						/* tp_setattr */
	0,						/* tp_compare */
	(reprfunc)delta_repr,				/* tp_repr */
	&delta_as_number,				/* tp_as_number */
	0,						/* tp_as_sequence */
	0,						/* tp_as_mapping */
	(hashfunc)delta_hash,				/* tp_hash */
	0,              				/* tp_call */
	(reprfunc)delta_str,				/* tp_str */
	PyObject_GenericGetAttr,			/* tp_getattro */
	0,						/* tp_setattro */
	0,						/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,	/* tp_flags */
	delta_doc,					/* tp_doc */
	0,						/* tp_traverse */
	0,						/* tp_clear */
	delta_richcompare,				/* tp_richcompare */
	0,						/* tp_weaklistoffset */
	0,						/* tp_iter */
	0,						/* tp_iternext */
	delta_methods,					/* tp_methods */
	delta_members,					/* tp_members */
	0,						/* tp_getset */
	0,						/* tp_base */
	0,						/* tp_dict */
	0,						/* tp_descr_get */
	0,						/* tp_descr_set */
	0,						/* tp_dictoffset */
	0,						/* tp_init */
	delta_new,					/* tp_new */
};

/*
 * PyDateTime_Date implementation.
 */

/* Accessor properties. */

static PyObject *
date_year(PyDateTime_Date *self, void *unused)
{
	return PyLong_FromLong(GET_YEAR(self));
}

static PyObject *
date_month(PyDateTime_Date *self, void *unused)
{
	return PyLong_FromLong(GET_MONTH(self));
}

static PyObject *
date_day(PyDateTime_Date *self, void *unused)
{
	return PyLong_FromLong(GET_DAY(self));
}

static PyGetSetDef date_getset[] = {
	{"year",        (getter)date_year},
	{"month",       (getter)date_month},
	{"day",         (getter)date_day},
	{NULL}
};

/* Constructors. */

static char *date_kws[] = {"year", "month", "day", NULL};

static PyObject *
date_new(PyTypeObject *type, PyObject *args, PyObject *kw)
{
	PyObject *self = NULL;
	PyObject *state;
	int year;
	int month;
	int day;

	/* Check for invocation from pickle with __getstate__ state */
	if (PyTuple_GET_SIZE(args) == 1 &&
	    PyString_Check(state = PyTuple_GET_ITEM(args, 0)) &&
	    PyString_GET_SIZE(state) == _PyDateTime_DATE_DATASIZE &&
	    MONTH_IS_SANE(PyString_AS_STRING(state)[2]))
	{
	    	PyDateTime_Date *me;

		me = PyObject_New(type);
		if (me != NULL) {
			char *pdata = PyString_AS_STRING(state);
			memcpy(me->data, pdata, _PyDateTime_DATE_DATASIZE);
			me->hashcode = -1;
		}
		return (PyObject *)me;
	}

	if (PyArg_ParseTupleAndKeywords(args, kw, "iii", date_kws,
					&year, &month, &day)) {
		if (check_date_args(year, month, day) < 0)
			return NULL;
		self = new_date_ex(year, month, day, type);
	}
	return self;
}

/* Return new date from localtime(t). */
static PyObject *
date_local_from_time_t(PyObject *cls, double ts)
{
	struct tm *tm;
	time_t t;
	PyObject *result = NULL;

	t = _PyTime_DoubleToTimet(ts);
	if (t == (time_t)-1 && PyErr_Occurred())
		return NULL;
	tm = localtime(&t);
	if (tm)
		result = PyObject_CallFunction(cls, "iii",
					       tm->tm_year + 1900,
					       tm->tm_mon + 1,
					       tm->tm_mday);
	else
		PyErr_SetString(PyExc_ValueError,
				"timestamp out of range for "
				"platform localtime() function");
	return result;
}

/* Return new date from current time.
 * We say this is equivalent to fromtimestamp(time.time()), and the
 * only way to be sure of that is to *call* time.time().  That's not
 * generally the same as calling C's time.
 */
static PyObject *
date_today(PyObject *cls, PyObject *dummy)
{
	PyObject *time;
	PyObject *result;

	time = time_time();
	if (time == NULL)
		return NULL;

	/* Note well:  today() is a class method, so this may not call
	 * date.fromtimestamp.  For example, it may call
	 * datetime.fromtimestamp.  That's why we need all the accuracy
	 * time.time() delivers; if someone were gonzo about optimization,
	 * date.today() could get away with plain C time().
	 */
	result = PyObject_CallMethod(cls, "fromtimestamp", "O", time);
	Py_DECREF(time);
	return result;
}

/* Return new date from given timestamp (Python timestamp -- a double). */
static PyObject *
date_fromtimestamp(PyObject *cls, PyObject *args)
{
	double timestamp;
	PyObject *result = NULL;

	if (PyArg_ParseTuple(args, "d:fromtimestamp", &timestamp))
		result = date_local_from_time_t(cls, timestamp);
	return result;
}

/* Return new date from proleptic Gregorian ordinal.  Raises ValueError if
 * the ordinal is out of range.
 */
static PyObject *
date_fromordinal(PyObject *cls, PyObject *args)
{
	PyObject *result = NULL;
	int ordinal;

	if (PyArg_ParseTuple(args, "i:fromordinal", &ordinal)) {
		int year;
		int month;
		int day;

		if (ordinal < 1)
			PyErr_SetString(PyExc_ValueError, "ordinal must be "
							  ">= 1");
		else {
			ord_to_ymd(ordinal, &year, &month, &day);
			result = PyObject_CallFunction(cls, "iii",
						       year, month, day);
		}
	}
	return result;
}

/*
 * Date arithmetic.
 */

/* date + timedelta -> date.  If arg negate is true, subtract the timedelta
 * instead.
 */
static PyObject *
add_date_timedelta(PyDateTime_Date *date, PyDateTime_Delta *delta, int negate)
{
	PyObject *result = NULL;
	int year = GET_YEAR(date);
	int month = GET_MONTH(date);
	int deltadays = GET_TD_DAYS(delta);
	/* C-level overflow is impossible because |deltadays| < 1e9. */
	int day = GET_DAY(date) + (negate ? -deltadays : deltadays);

	if (normalize_date(&year, &month, &day) >= 0)
		result = new_date(year, month, day);
	return result;
}

static PyObject *
date_add(PyObject *left, PyObject *right)
{
	if (PyDateTime_Check(left) || PyDateTime_Check(right)) {
		Py_INCREF(Py_NotImplemented);
		return Py_NotImplemented;
	}
	if (PyDate_Check(left)) {
		/* date + ??? */
		if (PyDelta_Check(right))
			/* date + delta */
			return add_date_timedelta((PyDateTime_Date *) left,
						  (PyDateTime_Delta *) right,
						  0);
	}
	else {
		/* ??? + date
		 * 'right' must be one of us, or we wouldn't have been called
		 */
		if (PyDelta_Check(left))
			/* delta + date */
			return add_date_timedelta((PyDateTime_Date *) right,
						  (PyDateTime_Delta *) left,
						  0);
	}
	Py_INCREF(Py_NotImplemented);
	return Py_NotImplemented;
}

static PyObject *
date_subtract(PyObject *left, PyObject *right)
{
	if (PyDateTime_Check(left) || PyDateTime_Check(right)) {
		Py_INCREF(Py_NotImplemented);
		return Py_NotImplemented;
	}
	if (PyDate_Check(left)) {
		if (PyDate_Check(right)) {
			/* date - date */
			int left_ord = ymd_to_ord(GET_YEAR(left),
						  GET_MONTH(left),
						  GET_DAY(left));
			int right_ord = ymd_to_ord(GET_YEAR(right),
						   GET_MONTH(right),
						   GET_DAY(right));
			return new_delta(left_ord - right_ord, 0, 0, 0);
		}
		if (PyDelta_Check(right)) {
			/* date - delta */
			return add_date_timedelta((PyDateTime_Date *) left,
						  (PyDateTime_Delta *) right,
						  1);
		}
	}
	Py_INCREF(Py_NotImplemented);
	return Py_NotImplemented;
}


/* Various ways to turn a date into a string. */

static PyObject *
date_repr(PyDateTime_Date *self)
{
	return PyUnicode_FromFormat("%s(%d, %d, %d)",
	                            Py_TYPE(self)->tp_name,
	                            GET_YEAR(self), GET_MONTH(self), GET_DAY(self));
}

static PyObject *
date_isoformat(PyDateTime_Date *self)
{
	return PyUnicode_FromFormat("%04d-%02d-%02d",
	                            GET_YEAR(self), GET_MONTH(self), GET_DAY(self));
}

/* str() calls the appropriate isoformat() method. */
static PyObject *
date_str(PyDateTime_Date *self)
{
	return PyObject_CallMethod((PyObject *)self, "isoformat", "()");
}


static PyObject *
date_ctime(PyDateTime_Date *self)
{
	return format_ctime(self, 0, 0, 0);
}

static PyObject *
date_strftime(PyDateTime_Date *self, PyObject *args, PyObject *kw)
{
	/* This method can be inherited, and needs to call the
	 * timetuple() method appropriate to self's class.
	 */
	PyObject *result;
	PyObject *format;
	PyObject *tuple;
	static char *keywords[] = {"format", NULL};

	if (! PyArg_ParseTupleAndKeywords(args, kw, "U:strftime", keywords,
					  &format))
		return NULL;

	tuple = PyObject_CallMethod((PyObject *)self, "timetuple", "()");
	if (tuple == NULL)
		return NULL;
	result = wrap_strftime((PyObject *)self, format, tuple,
			       (PyObject *)self);
	Py_DECREF(tuple);
	return result;
}

static PyObject *
date_format(PyDateTime_Date *self, PyObject *args)
{
	PyObject *format;

	if (!PyArg_ParseTuple(args, "U:__format__", &format))
		return NULL;

	/* if the format is zero length, return str(self) */
	if (PyUnicode_GetSize(format) == 0)
                return PyObject_Str((PyObject *)self);

        return PyObject_CallMethod((PyObject *)self, "strftime", "O", format);
}

/* ISO methods. */

static PyObject *
date_isoweekday(PyDateTime_Date *self)
{
	int dow = weekday(GET_YEAR(self), GET_MONTH(self), GET_DAY(self));

	return PyLong_FromLong(dow + 1);
}

static PyObject *
date_isocalendar(PyDateTime_Date *self)
{
	int  year         = GET_YEAR(self);
	int  week1_monday = iso_week1_monday(year);
	int today         = ymd_to_ord(year, GET_MONTH(self), GET_DAY(self));
	int  week;
	int  day;

	week = divmod(today - week1_monday, 7, &day);
	if (week < 0) {
		--year;
		week1_monday = iso_week1_monday(year);
		week = divmod(today - week1_monday, 7, &day);
	}
	else if (week >= 52 && today >= iso_week1_monday(year + 1)) {
		++year;
		week = 0;
	}
	return Py_BuildValue("iii", year, week + 1, day + 1);
}

/* Miscellaneous methods. */

static PyObject *
date_richcompare(PyObject *self, PyObject *other, int op)
{
	if (PyDate_Check(other)) {
		int diff = memcmp(((PyDateTime_Date *)self)->data,
				  ((PyDateTime_Date *)other)->data,
				  _PyDateTime_DATE_DATASIZE);
		return diff_to_bool(diff, op);
	}
	else {
		Py_INCREF(Py_NotImplemented);
		return Py_NotImplemented;
	}
}

static PyObject *
date_timetuple(PyDateTime_Date *self)
{
	return build_struct_time(GET_YEAR(self),
				 GET_MONTH(self),
				 GET_DAY(self),
				 0, 0, 0, -1);
}

static PyObject *
date_replace(PyDateTime_Date *self, PyObject *args, PyObject *kw)
{
	PyObject *clone;
	PyObject *tuple;
	int year = GET_YEAR(self);
	int month = GET_MONTH(self);
	int day = GET_DAY(self);

	if (! PyArg_ParseTupleAndKeywords(args, kw, "|iii:replace", date_kws,
					  &year, &month, &day))
		return NULL;
	tuple = Py_BuildValue("iii", year, month, day);
	if (tuple == NULL)
		return NULL;
	clone = date_new(Py_TYPE(self), tuple, NULL);
	Py_DECREF(tuple);
	return clone;
}

/*
	Borrowed from stringobject.c, originally it was string_hash()
*/
static long
generic_hash(unsigned char *data, int len)
{
	register unsigned char *p;
	register long x;

	p = (unsigned char *) data;
	x = *p << 7;
	while (--len >= 0)
		x = (1000003*x) ^ *p++;
	x ^= len;
	if (x == -1)
		x = -2;

	return x;
}


static PyObject *date_getstate(PyDateTime_Date *self);

static long
date_hash(PyDateTime_Date *self)
{
	if (self->hashcode == -1)
		self->hashcode = generic_hash(
			(unsigned char *)self->data, _PyDateTime_DATE_DATASIZE);

	return self->hashcode;
}

static PyObject *
date_toordinal(PyDateTime_Date *self)
{
	return PyLong_FromLong(ymd_to_ord(GET_YEAR(self), GET_MONTH(self),
					 GET_DAY(self)));
}

static PyObject *
date_weekday(PyDateTime_Date *self)
{
	int dow = weekday(GET_YEAR(self), GET_MONTH(self), GET_DAY(self));

	return PyLong_FromLong(dow);
}

/* Pickle support, a simple use of __reduce__. */

/* __getstate__ isn't exposed */
static PyObject *
date_getstate(PyDateTime_Date *self)
{
	PyObject* field;
	field = PyString_FromStringAndSize((char*)self->data,
					   _PyDateTime_DATE_DATASIZE);
	return Py_BuildValue("(N)", field);
}

static PyObject *
date_reduce(PyDateTime_Date *self, PyObject *arg)
{
	return Py_BuildValue("(ON)", Py_TYPE(self), date_getstate(self));
}

static PyMethodDef date_methods[] = {

	/* Class methods: */

	{"fromtimestamp", (PyCFunction)date_fromtimestamp, METH_VARARGS |
							   METH_CLASS,
	 PyDoc_STR("timestamp -> local date from a POSIX timestamp (like "
	 	   "time.time()).")},

	{"fromordinal", (PyCFunction)date_fromordinal,	METH_VARARGS |
							METH_CLASS,
	 PyDoc_STR("int -> date corresponding to a proleptic Gregorian "
	 	   "ordinal.")},

	{"today",         (PyCFunction)date_today,   METH_NOARGS | METH_CLASS,
	 PyDoc_STR("Current date or datetime:  same as "
	 	   "self.__class__.fromtimestamp(time.time()).")},

	/* Instance methods: */

	{"ctime",       (PyCFunction)date_ctime,        METH_NOARGS,
	 PyDoc_STR("Return ctime() style string.")},

	{"strftime",   	(PyCFunction)date_strftime,	METH_VARARGS | METH_KEYWORDS,
	 PyDoc_STR("format -> strftime() style string.")},

	{"__format__", 	(PyCFunction)date_format,	METH_VARARGS,
	 PyDoc_STR("Formats self with strftime.")},

	{"timetuple",   (PyCFunction)date_timetuple,    METH_NOARGS,
         PyDoc_STR("Return time tuple, compatible with time.localtime().")},

	{"isocalendar", (PyCFunction)date_isocalendar,  METH_NOARGS,
	 PyDoc_STR("Return a 3-tuple containing ISO year, week number, and "
	 	   "weekday.")},

	{"isoformat",   (PyCFunction)date_isoformat,	METH_NOARGS,
	 PyDoc_STR("Return string in ISO 8601 format, YYYY-MM-DD.")},

	{"isoweekday",  (PyCFunction)date_isoweekday,   METH_NOARGS,
	 PyDoc_STR("Return the day of the week represented by the date.\n"
	 	   "Monday == 1 ... Sunday == 7")},

	{"toordinal",   (PyCFunction)date_toordinal,    METH_NOARGS,
	 PyDoc_STR("Return proleptic Gregorian ordinal.  January 1 of year "
	 	   "1 is day 1.")},

	{"weekday",     (PyCFunction)date_weekday,      METH_NOARGS,
	 PyDoc_STR("Return the day of the week represented by the date.\n"
		   "Monday == 0 ... Sunday == 6")},

	{"replace",     (PyCFunction)date_replace,      METH_VARARGS | METH_KEYWORDS,
	 PyDoc_STR("Return date with new specified fields.")},

	{"__reduce__", (PyCFunction)date_reduce,        METH_NOARGS,
	 PyDoc_STR("__reduce__() -> (cls, state)")},

	{NULL,	NULL}
};

static char date_doc[] =
PyDoc_STR("date(year, month, day) --> date object");

static PyNumberMethods date_as_number = {
	date_add,					/* nb_add */
	date_subtract,					/* nb_subtract */
	0,						/* nb_multiply */
	0,						/* nb_remainder */
	0,						/* nb_divmod */
	0,						/* nb_power */
	0,						/* nb_negative */
	0,						/* nb_positive */
	0,						/* nb_absolute */
	0,						/* nb_bool */
};

static PyTypeObject PyDateTime_DateType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"datetime.date",				/* tp_name */
	sizeof(PyDateTime_Date),			/* tp_basicsize */
	0,						/* tp_itemsize */
	0,						/* tp_dealloc */
	0,						/* tp_print */
	0,						/* tp_getattr */
	0,						/* tp_setattr */
	0,						/* tp_compare */
	(reprfunc)date_repr,				/* tp_repr */
	&date_as_number,				/* tp_as_number */
	0,						/* tp_as_sequence */
	0,						/* tp_as_mapping */
	(hashfunc)date_hash,				/* tp_hash */
	0,              				/* tp_call */
	(reprfunc)date_str,				/* tp_str */
	PyObject_GenericGetAttr,			/* tp_getattro */
	0,						/* tp_setattro */
	0,						/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,	/* tp_flags */
	date_doc,					/* tp_doc */
	0,						/* tp_traverse */
	0,						/* tp_clear */
	date_richcompare,				/* tp_richcompare */
	0,						/* tp_weaklistoffset */
	0,						/* tp_iter */
	0,						/* tp_iternext */
	date_methods,					/* tp_methods */
	0,						/* tp_members */
	date_getset,					/* tp_getset */
	0,						/* tp_base */
	0,						/* tp_dict */
	0,						/* tp_descr_get */
	0,						/* tp_descr_set */
	0,						/* tp_dictoffset */
	0,						/* tp_init */
	date_new,					/* tp_new */
};

/*
 * PyDateTime_TZInfo implementation.
 */

/* This is a pure abstract base class, so doesn't do anything beyond
 * raising NotImplemented exceptions.  Real tzinfo classes need
 * to derive from this.  This is mostly for clarity, and for efficiency in
 * datetime and time constructors (their tzinfo arguments need to
 * be subclasses of this tzinfo class, which is easy and quick to check).
 *
 * Note:  For reasons having to do with pickling of subclasses, we have
 * to allow tzinfo objects to be instantiated.  This wasn't an issue
 * in the Python implementation (__init__() could raise NotImplementedError
 * there without ill effect), but doing so in the C implementation hit a
 * brick wall.
 */

static PyObject *
tzinfo_nogo(const char* methodname)
{
	PyErr_Format(PyExc_NotImplementedError,
		     "a tzinfo subclass must implement %s()",
		     methodname);
	return NULL;
}

/* Methods.  A subclass must implement these. */

static PyObject *
tzinfo_tzname(PyDateTime_TZInfo *self, PyObject *dt)
{
	return tzinfo_nogo("tzname");
}

static PyObject *
tzinfo_utcoffset(PyDateTime_TZInfo *self, PyObject *dt)
{
	return tzinfo_nogo("utcoffset");
}

static PyObject *
tzinfo_dst(PyDateTime_TZInfo *self, PyObject *dt)
{
	return tzinfo_nogo("dst");
}

static PyObject *
tzinfo_fromutc(PyDateTime_TZInfo *self, PyDateTime_DateTime *dt)
{
	int y, m, d, hh, mm, ss, us;

	PyObject *result;
	int off, dst;
	int none;
	int delta;

	if (! PyDateTime_Check(dt)) {
		PyErr_SetString(PyExc_TypeError,
				"fromutc: argument must be a datetime");
		return NULL;
	}
	if (! HASTZINFO(dt) || dt->tzinfo != (PyObject *)self) {
	    	PyErr_SetString(PyExc_ValueError, "fromutc: dt.tzinfo "
	    			"is not self");
	    	return NULL;
	}

	off = call_utcoffset(dt->tzinfo, (PyObject *)dt, &none);
	if (off == -1 && PyErr_Occurred())
		return NULL;
	if (none) {
		PyErr_SetString(PyExc_ValueError, "fromutc: non-None "
				"utcoffset() result required");
		return NULL;
	}

	dst = call_dst(dt->tzinfo, (PyObject *)dt, &none);
	if (dst == -1 && PyErr_Occurred())
		return NULL;
	if (none) {
		PyErr_SetString(PyExc_ValueError, "fromutc: non-None "
				"dst() result required");
		return NULL;
	}

	y = GET_YEAR(dt);
	m = GET_MONTH(dt);
	d = GET_DAY(dt);
	hh = DATE_GET_HOUR(dt);
	mm = DATE_GET_MINUTE(dt);
	ss = DATE_GET_SECOND(dt);
	us = DATE_GET_MICROSECOND(dt);

	delta = off - dst;
	mm += delta;
	if ((mm < 0 || mm >= 60) &&
	    normalize_datetime(&y, &m, &d, &hh, &mm, &ss, &us) < 0)
		return NULL;
	result = new_datetime(y, m, d, hh, mm, ss, us, dt->tzinfo);
	if (result == NULL)
		return result;

	dst = call_dst(dt->tzinfo, result, &none);
	if (dst == -1 && PyErr_Occurred())
		goto Fail;
	if (none)
		goto Inconsistent;
	if (dst == 0)
		return result;

	mm += dst;
	if ((mm < 0 || mm >= 60) &&
	    normalize_datetime(&y, &m, &d, &hh, &mm, &ss, &us) < 0)
		goto Fail;
	Py_DECREF(result);
	result = new_datetime(y, m, d, hh, mm, ss, us, dt->tzinfo);
	return result;

Inconsistent:
	PyErr_SetString(PyExc_ValueError, "fromutc: tz.dst() gave"
			"inconsistent results; cannot convert");

	/* fall thru to failure */
Fail:
	Py_DECREF(result);
	return NULL;
}

/*
 * Pickle support.  This is solely so that tzinfo subclasses can use
 * pickling -- tzinfo itself is supposed to be uninstantiable.
 */

static PyObject *
tzinfo_reduce(PyObject *self)
{
	PyObject *args, *state, *tmp;
	PyObject *getinitargs, *getstate;

	tmp = PyTuple_New(0);
	if (tmp == NULL)
		return NULL;

	getinitargs = PyObject_GetAttrString(self, "__getinitargs__");
	if (getinitargs != NULL) {
		args = PyObject_CallObject(getinitargs, tmp);
		Py_DECREF(getinitargs);
		if (args == NULL) {
			Py_DECREF(tmp);
			return NULL;
		}
	}
	else {
		PyErr_Clear();
		args = tmp;
		Py_INCREF(args);
	}

	getstate = PyObject_GetAttrString(self, "__getstate__");
	if (getstate != NULL) {
		state = PyObject_CallObject(getstate, tmp);
		Py_DECREF(getstate);
		if (state == NULL) {
			Py_DECREF(args);
			Py_DECREF(tmp);
			return NULL;
		}
	}
	else {
		PyObject **dictptr;
		PyErr_Clear();
		state = Py_None;
		dictptr = _PyObject_GetDictPtr(self);
		if (dictptr && *dictptr && PyDict_Size(*dictptr))
			state = *dictptr;
		Py_INCREF(state);
	}

	Py_DECREF(tmp);

	if (state == Py_None) {
		Py_DECREF(state);
		return Py_BuildValue("(ON)", Py_TYPE(self), args);
	}
	else
		return Py_BuildValue("(ONN)", Py_TYPE(self), args, state);
}

static PyMethodDef tzinfo_methods[] = {

	{"tzname",	(PyCFunction)tzinfo_tzname,		METH_O,
	 PyDoc_STR("datetime -> string name of time zone.")},

	{"utcoffset",	(PyCFunction)tzinfo_utcoffset,		METH_O,
	 PyDoc_STR("datetime -> minutes east of UTC (negative for "
	 	   "west of UTC).")},

	{"dst",		(PyCFunction)tzinfo_dst,		METH_O,
	 PyDoc_STR("datetime -> DST offset in minutes east of UTC.")},

	{"fromutc",	(PyCFunction)tzinfo_fromutc,		METH_O,
	 PyDoc_STR("datetime in UTC -> datetime in local time.")},

	{"__reduce__",  (PyCFunction)tzinfo_reduce,             METH_NOARGS,
	 PyDoc_STR("-> (cls, state)")},

	{NULL, NULL}
};

static char tzinfo_doc[] =
PyDoc_STR("Abstract base class for time zone info objects.");

static PyTypeObject PyDateTime_TZInfoType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"datetime.tzinfo",			/* tp_name */
	sizeof(PyDateTime_TZInfo),		/* tp_basicsize */
	0,					/* tp_itemsize */
	0,					/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,              			/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
	tzinfo_doc,				/* tp_doc */
	0,					/* tp_traverse */
	0,					/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	0,					/* tp_iter */
	0,					/* tp_iternext */
	tzinfo_methods,				/* tp_methods */
	0,					/* tp_members */
	0,					/* tp_getset */
	0,					/* tp_base */
	0,					/* tp_dict */
	0,					/* tp_descr_get */
	0,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	0,					/* tp_init */
	PyType_GenericNew,			/* tp_new */
};

/*
 * PyDateTime_Time implementation.
 */

/* Accessor properties.
 */

static PyObject *
time_hour(PyDateTime_Time *self, void *unused)
{
	return PyLong_FromLong(TIME_GET_HOUR(self));
}

static PyObject *
time_minute(PyDateTime_Time *self, void *unused)
{
	return PyLong_FromLong(TIME_GET_MINUTE(self));
}

/* The name time_second conflicted with some platform header file. */
static PyObject *
py_time_second(PyDateTime_Time *self, void *unused)
{
	return PyLong_FromLong(TIME_GET_SECOND(self));
}

static PyObject *
time_microsecond(PyDateTime_Time *self, void *unused)
{
	return PyLong_FromLong(TIME_GET_MICROSECOND(self));
}

static PyObject *
time_tzinfo(PyDateTime_Time *self, void *unused)
{
	PyObject *result = HASTZINFO(self) ? self->tzinfo : Py_None;
	Py_INCREF(result);
	return result;
}

static PyGetSetDef time_getset[] = {
	{"hour",        (getter)time_hour},
	{"minute",      (getter)time_minute},
	{"second",      (getter)py_time_second},
	{"microsecond", (getter)time_microsecond},
	{"tzinfo",	(getter)time_tzinfo},
	{NULL}
};

/*
 * Constructors.
 */

static char *time_kws[] = {"hour", "minute", "second", "microsecond",
			   "tzinfo", NULL};

static PyObject *
time_new(PyTypeObject *type, PyObject *args, PyObject *kw)
{
	PyObject *self = NULL;
	PyObject *state;
	int hour = 0;
	int minute = 0;
	int second = 0;
	int usecond = 0;
	PyObject *tzinfo = Py_None;

	/* Check for invocation from pickle with __getstate__ state */
	if (PyTuple_GET_SIZE(args) >= 1 &&
	    PyTuple_GET_SIZE(args) <= 2 &&
	    PyString_Check(state = PyTuple_GET_ITEM(args, 0)) &&
	    PyString_GET_SIZE(state) == _PyDateTime_TIME_DATASIZE &&
	    ((unsigned char) (PyString_AS_STRING(state)[0])) < 24)
	{
		PyDateTime_Time *me;
		char aware;

		if (PyTuple_GET_SIZE(args) == 2) {
			tzinfo = PyTuple_GET_ITEM(args, 1);
			if (check_tzinfo_subclass(tzinfo) < 0) {
				PyErr_SetString(PyExc_TypeError, "bad "
					"tzinfo state arg");
				return NULL;
			}
		}
		aware = (char)(tzinfo != Py_None);
		me = PyObject_New(type);
		if (me != NULL) {
			char *pdata = PyString_AS_STRING(state);

			memcpy(me->data, pdata, _PyDateTime_TIME_DATASIZE);
			me->hashcode = -1;
			me->hastzinfo = aware;
			if (aware) {
				Py_INCREF(tzinfo);
				me->tzinfo = tzinfo;
			}
		}
		return (PyObject *)me;
	}

	if (PyArg_ParseTupleAndKeywords(args, kw, "|iiiiO", time_kws,
					&hour, &minute, &second, &usecond,
					&tzinfo)) {
		if (check_time_args(hour, minute, second, usecond) < 0)
			return NULL;
		if (check_tzinfo_subclass(tzinfo) < 0)
			return NULL;
		self = new_time_ex(hour, minute, second, usecond, tzinfo,
				   type);
	}
	return self;
}

/*
 * Destructor.
 */

static void
time_dealloc(PyDateTime_Time *self)
{
	if (HASTZINFO(self)) {
		Py_XDECREF(self->tzinfo);
	}
	PyObject_Del(self);
}

/*
 * Indirect access to tzinfo methods.
 */

/* These are all METH_NOARGS, so don't need to check the arglist. */
static PyObject *
time_utcoffset(PyDateTime_Time *self, PyObject *unused) {
	return offset_as_timedelta(HASTZINFO(self) ? self->tzinfo : Py_None,
				   "utcoffset", Py_None);
}

static PyObject *
time_dst(PyDateTime_Time *self, PyObject *unused) {
	return offset_as_timedelta(HASTZINFO(self) ? self->tzinfo : Py_None,
				   "dst", Py_None);
}

static PyObject *
time_tzname(PyDateTime_Time *self, PyObject *unused) {
	return call_tzname(HASTZINFO(self) ? self->tzinfo : Py_None,
			   Py_None);
}

/*
 * Various ways to turn a time into a string.
 */

static PyObject *
time_repr(PyDateTime_Time *self)
{
	const char *type_name = Py_TYPE(self)->tp_name;
	int h = TIME_GET_HOUR(self);
	int m = TIME_GET_MINUTE(self);
	int s = TIME_GET_SECOND(self);
	int us = TIME_GET_MICROSECOND(self);
	PyObject *result = NULL;

	if (us)
		result = PyUnicode_FromFormat("%s(%d, %d, %d, %d)",
		                              type_name, h, m, s, us);
	else if (s)
		result = PyUnicode_FromFormat("%s(%d, %d, %d)",
		                              type_name, h, m, s);
	else
		result = PyUnicode_FromFormat("%s(%d, %d)", type_name, h, m);
	if (result != NULL && HASTZINFO(self))
		result = append_keyword_tzinfo(result, self->tzinfo);
	return result;
}

static PyObject *
time_str(PyDateTime_Time *self)
{
	return PyObject_CallMethod((PyObject *)self, "isoformat", "()");
}

static PyObject *
time_isoformat(PyDateTime_Time *self, PyObject *unused)
{
	char buf[100];
	PyObject *result;
	int us = TIME_GET_MICROSECOND(self);;

	if (us)
		result = PyUnicode_FromFormat("%02d:%02d:%02d.%06d",
		                              TIME_GET_HOUR(self),
		                              TIME_GET_MINUTE(self),
		                              TIME_GET_SECOND(self),
		                              us);
	else
		result = PyUnicode_FromFormat("%02d:%02d:%02d",
		                              TIME_GET_HOUR(self),
		                              TIME_GET_MINUTE(self),
		                              TIME_GET_SECOND(self));

	if (result == NULL || ! HASTZINFO(self) || self->tzinfo == Py_None)
		return result;

	/* We need to append the UTC offset. */
	if (format_utcoffset(buf, sizeof(buf), ":", self->tzinfo,
			     Py_None) < 0) {
		Py_DECREF(result);
		return NULL;
	}
	PyUnicode_AppendAndDel(&result, PyUnicode_FromString(buf));
	return result;
}

static PyObject *
time_strftime(PyDateTime_Time *self, PyObject *args, PyObject *kw)
{
	PyObject *result;
	PyObject *format;
	PyObject *tuple;
	static char *keywords[] = {"format", NULL};

	if (! PyArg_ParseTupleAndKeywords(args, kw, "U:strftime", keywords,
					  &format))
		return NULL;

	/* Python's strftime does insane things with the year part of the
	 * timetuple.  The year is forced to (the otherwise nonsensical)
	 * 1900 to worm around that.
	 */
	tuple = Py_BuildValue("iiiiiiiii",
		              1900, 1, 1, /* year, month, day */
			      TIME_GET_HOUR(self),
			      TIME_GET_MINUTE(self),
			      TIME_GET_SECOND(self),
			      0, 1, -1); /* weekday, daynum, dst */
	if (tuple == NULL)
		return NULL;
	assert(PyTuple_Size(tuple) == 9);
	result = wrap_strftime((PyObject *)self, format, tuple, Py_None);
	Py_DECREF(tuple);
	return result;
}

/*
 * Miscellaneous methods.
 */

static PyObject *
time_richcompare(PyObject *self, PyObject *other, int op)
{
	int diff;
	naivety n1, n2;
	int offset1, offset2;

	if (! PyTime_Check(other)) {
		Py_INCREF(Py_NotImplemented);
		return Py_NotImplemented;
	}
	if (classify_two_utcoffsets(self, &offset1, &n1, Py_None,
				    other, &offset2, &n2, Py_None) < 0)
		return NULL;
	assert(n1 != OFFSET_UNKNOWN && n2 != OFFSET_UNKNOWN);
	/* If they're both naive, or both aware and have the same offsets,
	 * we get off cheap.  Note that if they're both naive, offset1 ==
	 * offset2 == 0 at this point.
	 */
	if (n1 == n2 && offset1 == offset2) {
		diff = memcmp(((PyDateTime_Time *)self)->data,
			      ((PyDateTime_Time *)other)->data,
			      _PyDateTime_TIME_DATASIZE);
		return diff_to_bool(diff, op);
	}

	if (n1 == OFFSET_AWARE && n2 == OFFSET_AWARE) {
		assert(offset1 != offset2);	/* else last "if" handled it */
		/* Convert everything except microseconds to seconds.  These
		 * can't overflow (no more than the # of seconds in 2 days).
		 */
		offset1 = TIME_GET_HOUR(self) * 3600 +
			  (TIME_GET_MINUTE(self) - offset1) * 60 +
			  TIME_GET_SECOND(self);
		offset2 = TIME_GET_HOUR(other) * 3600 +
			  (TIME_GET_MINUTE(other) - offset2) * 60 +
			  TIME_GET_SECOND(other);
		diff = offset1 - offset2;
		if (diff == 0)
			diff = TIME_GET_MICROSECOND(self) -
			       TIME_GET_MICROSECOND(other);
		return diff_to_bool(diff, op);
	}

	assert(n1 != n2);
	PyErr_SetString(PyExc_TypeError,
			"can't compare offset-naive and "
			"offset-aware times");
	return NULL;
}

static long
time_hash(PyDateTime_Time *self)
{
	if (self->hashcode == -1) {
		naivety n;
		int offset;
		PyObject *temp;

		n = classify_utcoffset((PyObject *)self, Py_None, &offset);
		assert(n != OFFSET_UNKNOWN);
		if (n == OFFSET_ERROR)
			return -1;

		/* Reduce this to a hash of another object. */
		if (offset == 0) {
			self->hashcode = generic_hash(
				(unsigned char *)self->data, _PyDateTime_TIME_DATASIZE);
			return self->hashcode;
		}
		else {
			int hour;
			int minute;

			assert(n == OFFSET_AWARE);
			assert(HASTZINFO(self));
			hour = divmod(TIME_GET_HOUR(self) * 60 +
					TIME_GET_MINUTE(self) - offset,
				      60,
				      &minute);
			if (0 <= hour && hour < 24)
				temp = new_time(hour, minute,
						TIME_GET_SECOND(self),
						TIME_GET_MICROSECOND(self),
						Py_None);
			else
				temp = Py_BuildValue("iiii",
					   hour, minute,
					   TIME_GET_SECOND(self),
					   TIME_GET_MICROSECOND(self));
		}
		if (temp != NULL) {
			self->hashcode = PyObject_Hash(temp);
			Py_DECREF(temp);
		}
	}
	return self->hashcode;
}

static PyObject *
time_replace(PyDateTime_Time *self, PyObject *args, PyObject *kw)
{
	PyObject *clone;
	PyObject *tuple;
	int hh = TIME_GET_HOUR(self);
	int mm = TIME_GET_MINUTE(self);
	int ss = TIME_GET_SECOND(self);
	int us = TIME_GET_MICROSECOND(self);
	PyObject *tzinfo = HASTZINFO(self) ? self->tzinfo : Py_None;

	if (! PyArg_ParseTupleAndKeywords(args, kw, "|iiiiO:replace",
					  time_kws,
					  &hh, &mm, &ss, &us, &tzinfo))
		return NULL;
	tuple = Py_BuildValue("iiiiO", hh, mm, ss, us, tzinfo);
	if (tuple == NULL)
		return NULL;
	clone = time_new(Py_TYPE(self), tuple, NULL);
	Py_DECREF(tuple);
	return clone;
}

static int
time_bool(PyDateTime_Time *self)
{
	int offset;
	int none;

	if (TIME_GET_SECOND(self) || TIME_GET_MICROSECOND(self)) {
		/* Since utcoffset is in whole minutes, nothing can
		 * alter the conclusion that this is nonzero.
		 */
		return 1;
	}
	offset = 0;
	if (HASTZINFO(self) && self->tzinfo != Py_None) {
		offset = call_utcoffset(self->tzinfo, Py_None, &none);
		if (offset == -1 && PyErr_Occurred())
			return -1;
	}
	return (TIME_GET_MINUTE(self) - offset + TIME_GET_HOUR(self)*60) != 0;
}

/* Pickle support, a simple use of __reduce__. */

/* Let basestate be the non-tzinfo data string.
 * If tzinfo is None, this returns (basestate,), else (basestate, tzinfo).
 * So it's a tuple in any (non-error) case.
 * __getstate__ isn't exposed.
 */
static PyObject *
time_getstate(PyDateTime_Time *self)
{
	PyObject *basestate;
	PyObject *result = NULL;

	basestate =  PyString_FromStringAndSize((char *)self->data,
						_PyDateTime_TIME_DATASIZE);
	if (basestate != NULL) {
		if (! HASTZINFO(self) || self->tzinfo == Py_None)
			result = PyTuple_Pack(1, basestate);
		else
			result = PyTuple_Pack(2, basestate, self->tzinfo);
		Py_DECREF(basestate);
	}
	return result;
}

static PyObject *
time_reduce(PyDateTime_Time *self, PyObject *arg)
{
	return Py_BuildValue("(ON)", Py_TYPE(self), time_getstate(self));
}

static PyMethodDef time_methods[] = {

	{"isoformat",   (PyCFunction)time_isoformat,	METH_NOARGS,
	 PyDoc_STR("Return string in ISO 8601 format, HH:MM:SS[.mmmmmm]"
	 	   "[+HH:MM].")},

	{"strftime",   	(PyCFunction)time_strftime,	METH_VARARGS | METH_KEYWORDS,
	 PyDoc_STR("format -> strftime() style string.")},

	{"__format__", 	(PyCFunction)date_format,	METH_VARARGS,
	 PyDoc_STR("Formats self with strftime.")},

	{"utcoffset",	(PyCFunction)time_utcoffset,	METH_NOARGS,
	 PyDoc_STR("Return self.tzinfo.utcoffset(self).")},

	{"tzname",	(PyCFunction)time_tzname,	METH_NOARGS,
	 PyDoc_STR("Return self.tzinfo.tzname(self).")},

	{"dst",		(PyCFunction)time_dst,		METH_NOARGS,
	 PyDoc_STR("Return self.tzinfo.dst(self).")},

	{"replace",     (PyCFunction)time_replace,	METH_VARARGS | METH_KEYWORDS,
	 PyDoc_STR("Return time with new specified fields.")},

	{"__reduce__", (PyCFunction)time_reduce,        METH_NOARGS,
	 PyDoc_STR("__reduce__() -> (cls, state)")},

	{NULL,	NULL}
};

static char time_doc[] =
PyDoc_STR("time([hour[, minute[, second[, microsecond[, tzinfo]]]]]) --> a time object\n\
\n\
All arguments are optional. tzinfo may be None, or an instance of\n\
a tzinfo subclass. The remaining arguments may be ints or longs.\n");

static PyNumberMethods time_as_number = {
	0,					/* nb_add */
	0,					/* nb_subtract */
	0,					/* nb_multiply */
	0,					/* nb_remainder */
	0,					/* nb_divmod */
	0,					/* nb_power */
	0,					/* nb_negative */
	0,					/* nb_positive */
	0,					/* nb_absolute */
	(inquiry)time_bool,			/* nb_bool */
};

static PyTypeObject PyDateTime_TimeType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"datetime.time",			/* tp_name */
	sizeof(PyDateTime_Time),		/* tp_basicsize */
	0,					/* tp_itemsize */
	(destructor)time_dealloc,		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	(reprfunc)time_repr,			/* tp_repr */
	&time_as_number,			/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	(hashfunc)time_hash,			/* tp_hash */
	0,              			/* tp_call */
	(reprfunc)time_str,			/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
	time_doc,				/* tp_doc */
	0,					/* tp_traverse */
	0,					/* tp_clear */
	time_richcompare,			/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	0,					/* tp_iter */
	0,					/* tp_iternext */
	time_methods,				/* tp_methods */
	0,					/* tp_members */
	time_getset,				/* tp_getset */
	0,					/* tp_base */
	0,					/* tp_dict */
	0,					/* tp_descr_get */
	0,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	0,					/* tp_init */
	time_new,				/* tp_new */
};

/*
 * PyDateTime_DateTime implementation.
 */

/* Accessor properties.  Properties for day, month, and year are inherited
 * from date.
 */

static PyObject *
datetime_hour(PyDateTime_DateTime *self, void *unused)
{
	return PyLong_FromLong(DATE_GET_HOUR(self));
}

static PyObject *
datetime_minute(PyDateTime_DateTime *self, void *unused)
{
	return PyLong_FromLong(DATE_GET_MINUTE(self));
}

static PyObject *
datetime_second(PyDateTime_DateTime *self, void *unused)
{
	return PyLong_FromLong(DATE_GET_SECOND(self));
}

static PyObject *
datetime_microsecond(PyDateTime_DateTime *self, void *unused)
{
	return PyLong_FromLong(DATE_GET_MICROSECOND(self));
}

static PyObject *
datetime_tzinfo(PyDateTime_DateTime *self, void *unused)
{
	PyObject *result = HASTZINFO(self) ? self->tzinfo : Py_None;
	Py_INCREF(result);
	return result;
}

static PyGetSetDef datetime_getset[] = {
	{"hour",        (getter)datetime_hour},
	{"minute",      (getter)datetime_minute},
	{"second",      (getter)datetime_second},
	{"microsecond", (getter)datetime_microsecond},
	{"tzinfo",	(getter)datetime_tzinfo},
	{NULL}
};

/*
 * Constructors.
 */

static char *datetime_kws[] = {
	"year", "month", "day", "hour", "minute", "second",
	"microsecond", "tzinfo", NULL
};

static PyObject *
datetime_new(PyTypeObject *type, PyObject *args, PyObject *kw)
{
	PyObject *self = NULL;
	PyObject *state;
	int year;
	int month;
	int day;
	int hour = 0;
	int minute = 0;
	int second = 0;
	int usecond = 0;
	PyObject *tzinfo = Py_None;

	/* Check for invocation from pickle with __getstate__ state */
	if (PyTuple_GET_SIZE(args) >= 1 &&
	    PyTuple_GET_SIZE(args) <= 2 &&
	    PyString_Check(state = PyTuple_GET_ITEM(args, 0)) &&
	    PyString_GET_SIZE(state) == _PyDateTime_DATETIME_DATASIZE &&
	    MONTH_IS_SANE(PyString_AS_STRING(state)[2]))
	{
		PyDateTime_DateTime *me;
		char aware;

		if (PyTuple_GET_SIZE(args) == 2) {
			tzinfo = PyTuple_GET_ITEM(args, 1);
			if (check_tzinfo_subclass(tzinfo) < 0) {
				PyErr_SetString(PyExc_TypeError, "bad "
					"tzinfo state arg");
				return NULL;
			}
		}
		aware = (char)(tzinfo != Py_None);
		me = PyObject_New(type);
		if (me != NULL) {
			char *pdata = PyString_AS_STRING(state);

			memcpy(me->data, pdata, _PyDateTime_DATETIME_DATASIZE);
			me->hashcode = -1;
			me->hastzinfo = aware;
			if (aware) {
				Py_INCREF(tzinfo);
				me->tzinfo = tzinfo;
			}
		}
		return (PyObject *)me;
	}

	if (PyArg_ParseTupleAndKeywords(args, kw, "iii|iiiiO", datetime_kws,
					&year, &month, &day, &hour, &minute,
					&second, &usecond, &tzinfo)) {
		if (check_date_args(year, month, day) < 0)
			return NULL;
		if (check_time_args(hour, minute, second, usecond) < 0)
			return NULL;
		if (check_tzinfo_subclass(tzinfo) < 0)
			return NULL;
		self = new_datetime_ex(year, month, day,
				    	hour, minute, second, usecond,
				    	tzinfo, type);
	}
	return self;
}

/* TM_FUNC is the shared type of localtime() and gmtime(). */
typedef struct tm *(*TM_FUNC)(const time_t *timer);

/* Internal helper.
 * Build datetime from a time_t and a distinct count of microseconds.
 * Pass localtime or gmtime for f, to control the interpretation of timet.
 */
static PyObject *
datetime_from_timet_and_us(PyObject *cls, TM_FUNC f, time_t timet, int us,
			   PyObject *tzinfo)
{
	struct tm *tm;
	PyObject *result = NULL;

	tm = f(&timet);
	if (tm) {
		/* The platform localtime/gmtime may insert leap seconds,
		 * indicated by tm->tm_sec > 59.  We don't care about them,
		 * except to the extent that passing them on to the datetime
		 * constructor would raise ValueError for a reason that
		 * made no sense to the user.
		 */
		if (tm->tm_sec > 59)
			tm->tm_sec = 59;
		result = PyObject_CallFunction(cls, "iiiiiiiO",
					       tm->tm_year + 1900,
					       tm->tm_mon + 1,
					       tm->tm_mday,
					       tm->tm_hour,
					       tm->tm_min,
					       tm->tm_sec,
					       us,
					       tzinfo);
	}
	else
		PyErr_SetString(PyExc_ValueError,
				"timestamp out of range for "
				"platform localtime()/gmtime() function");
	return result;
}

/* Internal helper.
 * Build datetime from a Python timestamp.  Pass localtime or gmtime for f,
 * to control the interpretation of the timestamp.  Since a double doesn't
 * have enough bits to cover a datetime's full range of precision, it's
 * better to call datetime_from_timet_and_us provided you have a way
 * to get that much precision (e.g., C time() isn't good enough).
 */
static PyObject *
datetime_from_timestamp(PyObject *cls, TM_FUNC f, double timestamp,
			PyObject *tzinfo)
{
	time_t timet;
	double fraction;
	int us;

	timet = _PyTime_DoubleToTimet(timestamp);
	if (timet == (time_t)-1 && PyErr_Occurred())
		return NULL;
	fraction = timestamp - (double)timet;
	us = (int)round_to_long(fraction * 1e6);
	if (us < 0) {
		/* Truncation towards zero is not what we wanted
		   for negative numbers (Python's mod semantics) */
		timet -= 1;
		us += 1000000;
	}
	/* If timestamp is less than one microsecond smaller than a
	 * full second, round up. Otherwise, ValueErrors are raised
	 * for some floats. */
	if (us == 1000000) {
		timet += 1;
		us = 0;
	}
	return datetime_from_timet_and_us(cls, f, timet, us, tzinfo);
}

/* Internal helper.
 * Build most accurate possible datetime for current time.  Pass localtime or
 * gmtime for f as appropriate.
 */
static PyObject *
datetime_best_possible(PyObject *cls, TM_FUNC f, PyObject *tzinfo)
{
#ifdef HAVE_GETTIMEOFDAY
	struct timeval t;

#ifdef GETTIMEOFDAY_NO_TZ
	gettimeofday(&t);
#else
	gettimeofday(&t, (struct timezone *)NULL);
#endif
	return datetime_from_timet_and_us(cls, f, t.tv_sec, (int)t.tv_usec,
					  tzinfo);

#else	/* ! HAVE_GETTIMEOFDAY */
	/* No flavor of gettimeofday exists on this platform.  Python's
	 * time.time() does a lot of other platform tricks to get the
	 * best time it can on the platform, and we're not going to do
	 * better than that (if we could, the better code would belong
	 * in time.time()!)  We're limited by the precision of a double,
	 * though.
	 */
	PyObject *time;
	double dtime;

	time = time_time();
    	if (time == NULL)
    		return NULL;
	dtime = PyFloat_AsDouble(time);
	Py_DECREF(time);
	if (dtime == -1.0 && PyErr_Occurred())
		return NULL;
	return datetime_from_timestamp(cls, f, dtime, tzinfo);
#endif	/* ! HAVE_GETTIMEOFDAY */
}

/* Return best possible local time -- this isn't constrained by the
 * precision of a timestamp.
 */
static PyObject *
datetime_now(PyObject *cls, PyObject *args, PyObject *kw)
{
	PyObject *self;
	PyObject *tzinfo = Py_None;
	static char *keywords[] = {"tz", NULL};

	if (! PyArg_ParseTupleAndKeywords(args, kw, "|O:now", keywords,
					  &tzinfo))
		return NULL;
	if (check_tzinfo_subclass(tzinfo) < 0)
		return NULL;

	self = datetime_best_possible(cls,
				      tzinfo == Py_None ? localtime : gmtime,
				      tzinfo);
	if (self != NULL && tzinfo != Py_None) {
		/* Convert UTC to tzinfo's zone. */
		PyObject *temp = self;
		self = PyObject_CallMethod(tzinfo, "fromutc", "O", self);
		Py_DECREF(temp);
	}
	return self;
}

/* Return best possible UTC time -- this isn't constrained by the
 * precision of a timestamp.
 */
static PyObject *
datetime_utcnow(PyObject *cls, PyObject *dummy)
{
	return datetime_best_possible(cls, gmtime, Py_None);
}

/* Return new local datetime from timestamp (Python timestamp -- a double). */
static PyObject *
datetime_fromtimestamp(PyObject *cls, PyObject *args, PyObject *kw)
{
	PyObject *self;
	double timestamp;
	PyObject *tzinfo = Py_None;
	static char *keywords[] = {"timestamp", "tz", NULL};

	if (! PyArg_ParseTupleAndKeywords(args, kw, "d|O:fromtimestamp",
					  keywords, &timestamp, &tzinfo))
		return NULL;
	if (check_tzinfo_subclass(tzinfo) < 0)
		return NULL;

	self = datetime_from_timestamp(cls,
				       tzinfo == Py_None ? localtime : gmtime,
				       timestamp,
				       tzinfo);
	if (self != NULL && tzinfo != Py_None) {
		/* Convert UTC to tzinfo's zone. */
		PyObject *temp = self;
		self = PyObject_CallMethod(tzinfo, "fromutc", "O", self);
		Py_DECREF(temp);
	}
	return self;
}

/* Return new UTC datetime from timestamp (Python timestamp -- a double). */
static PyObject *
datetime_utcfromtimestamp(PyObject *cls, PyObject *args)
{
	double timestamp;
	PyObject *result = NULL;

	if (PyArg_ParseTuple(args, "d:utcfromtimestamp", &timestamp))
		result = datetime_from_timestamp(cls, gmtime, timestamp,
						 Py_None);
	return result;
}

/* Return new datetime from time.strptime(). */
static PyObject *
datetime_strptime(PyObject *cls, PyObject *args)
{
	static PyObject *module = NULL;
	PyObject *result = NULL, *obj, *st = NULL, *frac = NULL;
        const Py_UNICODE *string, *format;

	if (!PyArg_ParseTuple(args, "uu:strptime", &string, &format))
		return NULL;

	if (module == NULL &&
	    (module = PyImport_ImportModuleNoBlock("_strptime")) == NULL)
		return NULL;

	/* _strptime._strptime returns a two-element tuple.  The first
	   element is a time.struct_time object.  The second is the
	   microseconds (which are not defined for time.struct_time). */
	obj = PyObject_CallMethod(module, "_strptime", "uu", string, format);
	if (obj != NULL) {
		int i, good_timetuple = 1;
		long int ia[7];
		if (PySequence_Check(obj) && PySequence_Size(obj) == 2) {
			st = PySequence_GetItem(obj, 0);
			frac = PySequence_GetItem(obj, 1);
			if (st == NULL || frac == NULL)
				good_timetuple = 0;
			/* copy y/m/d/h/m/s values out of the
			   time.struct_time */
			if (good_timetuple &&
			    PySequence_Check(st) &&
			    PySequence_Size(st) >= 6) {
				for (i=0; i < 6; i++) {
					PyObject *p = PySequence_GetItem(st, i);
					if (p == NULL) {
						good_timetuple = 0;
						break;
					}
					if (PyLong_Check(p))
						ia[i] = PyLong_AsLong(p);
					else
						good_timetuple = 0;
					Py_DECREF(p);
				}
/*				if (PyLong_CheckExact(p)) {
					ia[i] = PyLong_AsLongAndOverflow(p, &overflow);
					if (overflow)
						good_timetuple = 0;
				}
				else
					good_timetuple = 0;
				Py_DECREF(p);
*/			}
			else
				good_timetuple = 0;
			/* follow that up with a little dose of microseconds */
			if (PyLong_Check(frac))
				ia[6] = PyLong_AsLong(frac);
			else
				good_timetuple = 0;
		}
		else
			good_timetuple = 0;
		if (good_timetuple)
			result = PyObject_CallFunction(cls, "iiiiiii",
						       ia[0], ia[1], ia[2],
						       ia[3], ia[4], ia[5],
						       ia[6]);
		else
			PyErr_SetString(PyExc_ValueError,
				"unexpected value from _strptime._strptime");
	}
	Py_XDECREF(obj);
	Py_XDECREF(st);
	Py_XDECREF(frac);
	return result;
}

/* Return new datetime from date/datetime and time arguments. */
static PyObject *
datetime_combine(PyObject *cls, PyObject *args, PyObject *kw)
{
 	static char *keywords[] = {"date", "time", NULL};
	PyObject *date;
	PyObject *time;
	PyObject *result = NULL;

	if (PyArg_ParseTupleAndKeywords(args, kw, "O!O!:combine", keywords,
					&PyDateTime_DateType, &date,
					&PyDateTime_TimeType, &time)) {
		PyObject *tzinfo = Py_None;

		if (HASTZINFO(time))
			tzinfo = ((PyDateTime_Time *)time)->tzinfo;
		result = PyObject_CallFunction(cls, "iiiiiiiO",
						GET_YEAR(date),
				    		GET_MONTH(date),
						GET_DAY(date),
				    		TIME_GET_HOUR(time),
				    		TIME_GET_MINUTE(time),
				    		TIME_GET_SECOND(time),
				    		TIME_GET_MICROSECOND(time),
				    		tzinfo);
	}
	return result;
}

/*
 * Destructor.
 */

static void
datetime_dealloc(PyDateTime_DateTime *self)
{
	if (HASTZINFO(self)) {
		Py_XDECREF(self->tzinfo);
	}
	PyObject_Del(self);
}

/*
 * Indirect access to tzinfo methods.
 */

/* These are all METH_NOARGS, so don't need to check the arglist. */
static PyObject *
datetime_utcoffset(PyDateTime_DateTime *self, PyObject *unused) {
	return offset_as_timedelta(HASTZINFO(self) ? self->tzinfo : Py_None,
				   "utcoffset", (PyObject *)self);
}

static PyObject *
datetime_dst(PyDateTime_DateTime *self, PyObject *unused) {
	return offset_as_timedelta(HASTZINFO(self) ? self->tzinfo : Py_None,
				   "dst", (PyObject *)self);
}

static PyObject *
datetime_tzname(PyDateTime_DateTime *self, PyObject *unused) {
	return call_tzname(HASTZINFO(self) ? self->tzinfo : Py_None,
			   (PyObject *)self);
}

/*
 * datetime arithmetic.
 */

/* factor must be 1 (to add) or -1 (to subtract).  The result inherits
 * the tzinfo state of date.
 */
static PyObject *
add_datetime_timedelta(PyDateTime_DateTime *date, PyDateTime_Delta *delta,
		       int factor)
{
	/* Note that the C-level additions can't overflow, because of
	 * invariant bounds on the member values.
	 */
	int year = GET_YEAR(date);
	int month = GET_MONTH(date);
	int day = GET_DAY(date) + GET_TD_DAYS(delta) * factor;
	int hour = DATE_GET_HOUR(date);
	int minute = DATE_GET_MINUTE(date);
	int second = DATE_GET_SECOND(date) + GET_TD_SECONDS(delta) * factor;
	int microsecond = DATE_GET_MICROSECOND(date) +
			  GET_TD_MICROSECONDS(delta) * factor;

	assert(factor == 1 || factor == -1);
	if (normalize_datetime(&year, &month, &day,
			       &hour, &minute, &second, &microsecond) < 0)
		return NULL;
	else
		return new_datetime(year, month, day,
				    hour, minute, second, microsecond,
				    HASTZINFO(date) ? date->tzinfo : Py_None);
}

static PyObject *
datetime_add(PyObject *left, PyObject *right)
{
	if (PyDateTime_Check(left)) {
		/* datetime + ??? */
		if (PyDelta_Check(right))
			/* datetime + delta */
			return add_datetime_timedelta(
					(PyDateTime_DateTime *)left,
					(PyDateTime_Delta *)right,
					1);
	}
	else if (PyDelta_Check(left)) {
		/* delta + datetime */
		return add_datetime_timedelta((PyDateTime_DateTime *) right,
					      (PyDateTime_Delta *) left,
					      1);
	}
	Py_INCREF(Py_NotImplemented);
	return Py_NotImplemented;
}

static PyObject *
datetime_subtract(PyObject *left, PyObject *right)
{
	PyObject *result = Py_NotImplemented;

	if (PyDateTime_Check(left)) {
		/* datetime - ??? */
		if (PyDateTime_Check(right)) {
			/* datetime - datetime */
			naivety n1, n2;
			int offset1, offset2;
			int delta_d, delta_s, delta_us;

			if (classify_two_utcoffsets(left, &offset1, &n1, left,
						    right, &offset2, &n2,
						    right) < 0)
				return NULL;
			assert(n1 != OFFSET_UNKNOWN && n2 != OFFSET_UNKNOWN);
			if (n1 != n2) {
				PyErr_SetString(PyExc_TypeError,
					"can't subtract offset-naive and "
					"offset-aware datetimes");
				return NULL;
			}
			delta_d = ymd_to_ord(GET_YEAR(left),
					     GET_MONTH(left),
					     GET_DAY(left)) -
				  ymd_to_ord(GET_YEAR(right),
					     GET_MONTH(right),
					     GET_DAY(right));
			/* These can't overflow, since the values are
			 * normalized.  At most this gives the number of
			 * seconds in one day.
			 */
			delta_s = (DATE_GET_HOUR(left) -
				   DATE_GET_HOUR(right)) * 3600 +
			          (DATE_GET_MINUTE(left) -
			           DATE_GET_MINUTE(right)) * 60 +
				  (DATE_GET_SECOND(left) -
				   DATE_GET_SECOND(right));
			delta_us = DATE_GET_MICROSECOND(left) -
				   DATE_GET_MICROSECOND(right);
			/* (left - offset1) - (right - offset2) =
			 * (left - right) + (offset2 - offset1)
			 */
			delta_s += (offset2 - offset1) * 60;
			result = new_delta(delta_d, delta_s, delta_us, 1);
		}
		else if (PyDelta_Check(right)) {
			/* datetime - delta */
			result = add_datetime_timedelta(
					(PyDateTime_DateTime *)left,
					(PyDateTime_Delta *)right,
					-1);
		}
	}

	if (result == Py_NotImplemented)
		Py_INCREF(result);
	return result;
}

/* Various ways to turn a datetime into a string. */

static PyObject *
datetime_repr(PyDateTime_DateTime *self)
{
	const char *type_name = Py_TYPE(self)->tp_name;
	PyObject *baserepr;

	if (DATE_GET_MICROSECOND(self)) {
		baserepr = PyUnicode_FromFormat(
			      "%s(%d, %d, %d, %d, %d, %d, %d)",
			      type_name,
			      GET_YEAR(self), GET_MONTH(self), GET_DAY(self),
			      DATE_GET_HOUR(self), DATE_GET_MINUTE(self),
			      DATE_GET_SECOND(self),
			      DATE_GET_MICROSECOND(self));
	}
	else if (DATE_GET_SECOND(self)) {
		baserepr = PyUnicode_FromFormat(
			      "%s(%d, %d, %d, %d, %d, %d)",
			      type_name,
			      GET_YEAR(self), GET_MONTH(self), GET_DAY(self),
			      DATE_GET_HOUR(self), DATE_GET_MINUTE(self),
			      DATE_GET_SECOND(self));
	}
	else {
		baserepr = PyUnicode_FromFormat(
			      "%s(%d, %d, %d, %d, %d)",
			      type_name,
			      GET_YEAR(self), GET_MONTH(self), GET_DAY(self),
			      DATE_GET_HOUR(self), DATE_GET_MINUTE(self));
	}
	if (baserepr == NULL || ! HASTZINFO(self))
		return baserepr;
	return append_keyword_tzinfo(baserepr, self->tzinfo);
}

static PyObject *
datetime_str(PyDateTime_DateTime *self)
{
	return PyObject_CallMethod((PyObject *)self, "isoformat", "(s)", " ");
}

static PyObject *
datetime_isoformat(PyDateTime_DateTime *self, PyObject *args, PyObject *kw)
{
	int sep = 'T';
	static char *keywords[] = {"sep", NULL};
	char buffer[100];
	PyObject *result;
	int us = DATE_GET_MICROSECOND(self);

	if (!PyArg_ParseTupleAndKeywords(args, kw, "|C:isoformat", keywords, &sep))
		return NULL;
	if (us)
		result = PyUnicode_FromFormat("%04d-%02d-%02d%c%02d:%02d:%02d.%06d",
		                              GET_YEAR(self), GET_MONTH(self),
		                              GET_DAY(self), (int)sep,
		                              DATE_GET_HOUR(self), DATE_GET_MINUTE(self),
		                              DATE_GET_SECOND(self), us);
	else
		result = PyUnicode_FromFormat("%04d-%02d-%02d%c%02d:%02d:%02d",
		                              GET_YEAR(self), GET_MONTH(self),
		                              GET_DAY(self), (int)sep,
		                              DATE_GET_HOUR(self), DATE_GET_MINUTE(self),
		                              DATE_GET_SECOND(self));

	if (!result || !HASTZINFO(self))
		return result;

	/* We need to append the UTC offset. */
	if (format_utcoffset(buffer, sizeof(buffer), ":", self->tzinfo,
			     (PyObject *)self) < 0) {
		Py_DECREF(result);
		return NULL;
	}
	PyUnicode_AppendAndDel(&result, PyUnicode_FromString(buffer));
	return result;
}

static PyObject *
datetime_ctime(PyDateTime_DateTime *self)
{
	return format_ctime((PyDateTime_Date *)self,
			    DATE_GET_HOUR(self),
			    DATE_GET_MINUTE(self),
			    DATE_GET_SECOND(self));
}

/* Miscellaneous methods. */

static PyObject *
datetime_richcompare(PyObject *self, PyObject *other, int op)
{
	int diff;
	naivety n1, n2;
	int offset1, offset2;

	if (! PyDateTime_Check(other)) {
		if (PyDate_Check(other)) {
			/* Prevent invocation of date_richcompare.  We want to
			   return NotImplemented here to give the other object
			   a chance.  But since DateTime is a subclass of
			   Date, if the other object is a Date, it would
			   compute an ordering based on the date part alone,
			   and we don't want that.  So force unequal or
			   uncomparable here in that case. */
			if (op == Py_EQ)
				Py_RETURN_FALSE;
			if (op == Py_NE)
				Py_RETURN_TRUE;
			return cmperror(self, other);
		}
		Py_INCREF(Py_NotImplemented);
		return Py_NotImplemented;
	}

	if (classify_two_utcoffsets(self, &offset1, &n1, self,
				    other, &offset2, &n2, other) < 0)
		return NULL;
	assert(n1 != OFFSET_UNKNOWN && n2 != OFFSET_UNKNOWN);
 	/* If they're both naive, or both aware and have the same offsets,
	 * we get off cheap.  Note that if they're both naive, offset1 ==
	 * offset2 == 0 at this point.
	 */
	if (n1 == n2 && offset1 == offset2) {
		diff = memcmp(((PyDateTime_DateTime *)self)->data,
			      ((PyDateTime_DateTime *)other)->data,
			      _PyDateTime_DATETIME_DATASIZE);
		return diff_to_bool(diff, op);
	}

	if (n1 == OFFSET_AWARE && n2 == OFFSET_AWARE) {
		PyDateTime_Delta *delta;

		assert(offset1 != offset2);	/* else last "if" handled it */
		delta = (PyDateTime_Delta *)datetime_subtract((PyObject *)self,
							       other);
		if (delta == NULL)
			return NULL;
		diff = GET_TD_DAYS(delta);
		if (diff == 0)
			diff = GET_TD_SECONDS(delta) |
			       GET_TD_MICROSECONDS(delta);
		Py_DECREF(delta);
		return diff_to_bool(diff, op);
	}

	assert(n1 != n2);
	PyErr_SetString(PyExc_TypeError,
			"can't compare offset-naive and "
			"offset-aware datetimes");
	return NULL;
}

static long
datetime_hash(PyDateTime_DateTime *self)
{
	if (self->hashcode == -1) {
		naivety n;
		int offset;
		PyObject *temp;

		n = classify_utcoffset((PyObject *)self, (PyObject *)self,
				       &offset);
		assert(n != OFFSET_UNKNOWN);
		if (n == OFFSET_ERROR)
			return -1;

		/* Reduce this to a hash of another object. */
		if (n == OFFSET_NAIVE) {
			self->hashcode = generic_hash(
				(unsigned char *)self->data, _PyDateTime_DATETIME_DATASIZE);
			return self->hashcode;
		}
		else {
			int days;
			int seconds;

			assert(n == OFFSET_AWARE);
			assert(HASTZINFO(self));
			days = ymd_to_ord(GET_YEAR(self),
					  GET_MONTH(self),
					  GET_DAY(self));
			seconds = DATE_GET_HOUR(self) * 3600 +
				  (DATE_GET_MINUTE(self) - offset) * 60 +
				  DATE_GET_SECOND(self);
			temp = new_delta(days,
					 seconds,
					 DATE_GET_MICROSECOND(self),
					 1);
		}
		if (temp != NULL) {
			self->hashcode = PyObject_Hash(temp);
			Py_DECREF(temp);
		}
	}
	return self->hashcode;
}

static PyObject *
datetime_replace(PyDateTime_DateTime *self, PyObject *args, PyObject *kw)
{
	PyObject *clone;
	PyObject *tuple;
	int y = GET_YEAR(self);
	int m = GET_MONTH(self);
	int d = GET_DAY(self);
	int hh = DATE_GET_HOUR(self);
	int mm = DATE_GET_MINUTE(self);
	int ss = DATE_GET_SECOND(self);
	int us = DATE_GET_MICROSECOND(self);
	PyObject *tzinfo = HASTZINFO(self) ? self->tzinfo : Py_None;

	if (! PyArg_ParseTupleAndKeywords(args, kw, "|iiiiiiiO:replace",
					  datetime_kws,
					  &y, &m, &d, &hh, &mm, &ss, &us,
					  &tzinfo))
		return NULL;
	tuple = Py_BuildValue("iiiiiiiO", y, m, d, hh, mm, ss, us, tzinfo);
	if (tuple == NULL)
		return NULL;
	clone = datetime_new(Py_TYPE(self), tuple, NULL);
	Py_DECREF(tuple);
	return clone;
}

static PyObject *
datetime_astimezone(PyDateTime_DateTime *self, PyObject *args, PyObject *kw)
{
	int y, m, d, hh, mm, ss, us;
	PyObject *result;
	int offset, none;

	PyObject *tzinfo;
	static char *keywords[] = {"tz", NULL};

	if (! PyArg_ParseTupleAndKeywords(args, kw, "O!:astimezone", keywords,
					  &PyDateTime_TZInfoType, &tzinfo))
		return NULL;

        if (!HASTZINFO(self) || self->tzinfo == Py_None)
        	goto NeedAware;

        /* Conversion to self's own time zone is a NOP. */
	if (self->tzinfo == tzinfo) {
		Py_INCREF(self);
		return (PyObject *)self;
	}

        /* Convert self to UTC. */
        offset = call_utcoffset(self->tzinfo, (PyObject *)self, &none);
        if (offset == -1 && PyErr_Occurred())
        	return NULL;
        if (none)
        	goto NeedAware;

	y = GET_YEAR(self);
	m = GET_MONTH(self);
	d = GET_DAY(self);
	hh = DATE_GET_HOUR(self);
	mm = DATE_GET_MINUTE(self);
	ss = DATE_GET_SECOND(self);
	us = DATE_GET_MICROSECOND(self);

	mm -= offset;
	if ((mm < 0 || mm >= 60) &&
	    normalize_datetime(&y, &m, &d, &hh, &mm, &ss, &us) < 0)
		return NULL;

	/* Attach new tzinfo and let fromutc() do the rest. */
	result = new_datetime(y, m, d, hh, mm, ss, us, tzinfo);
	if (result != NULL) {
		PyObject *temp = result;

		result = PyObject_CallMethod(tzinfo, "fromutc", "O", temp);
		Py_DECREF(temp);
	}
	return result;

NeedAware:
	PyErr_SetString(PyExc_ValueError, "astimezone() cannot be applied to "
					  "a naive datetime");
	return NULL;
}

static PyObject *
datetime_timetuple(PyDateTime_DateTime *self)
{
	int dstflag = -1;

	if (HASTZINFO(self) && self->tzinfo != Py_None) {
		int none;

		dstflag = call_dst(self->tzinfo, (PyObject *)self, &none);
		if (dstflag == -1 && PyErr_Occurred())
			return NULL;

		if (none)
			dstflag = -1;
		else if (dstflag != 0)
			dstflag = 1;

	}
	return build_struct_time(GET_YEAR(self),
				 GET_MONTH(self),
				 GET_DAY(self),
				 DATE_GET_HOUR(self),
				 DATE_GET_MINUTE(self),
				 DATE_GET_SECOND(self),
				 dstflag);
}

static PyObject *
datetime_getdate(PyDateTime_DateTime *self)
{
	return new_date(GET_YEAR(self),
			GET_MONTH(self),
			GET_DAY(self));
}

static PyObject *
datetime_gettime(PyDateTime_DateTime *self)
{
	return new_time(DATE_GET_HOUR(self),
			DATE_GET_MINUTE(self),
			DATE_GET_SECOND(self),
			DATE_GET_MICROSECOND(self),
			Py_None);
}

static PyObject *
datetime_gettimetz(PyDateTime_DateTime *self)
{
	return new_time(DATE_GET_HOUR(self),
			DATE_GET_MINUTE(self),
			DATE_GET_SECOND(self),
			DATE_GET_MICROSECOND(self),
			HASTZINFO(self) ? self->tzinfo : Py_None);
}

static PyObject *
datetime_utctimetuple(PyDateTime_DateTime *self)
{
	int y = GET_YEAR(self);
	int m = GET_MONTH(self);
	int d = GET_DAY(self);
	int hh = DATE_GET_HOUR(self);
	int mm = DATE_GET_MINUTE(self);
	int ss = DATE_GET_SECOND(self);
	int us = 0;	/* microseconds are ignored in a timetuple */
	int offset = 0;

	if (HASTZINFO(self) && self->tzinfo != Py_None) {
		int none;

		offset = call_utcoffset(self->tzinfo, (PyObject *)self, &none);
		if (offset == -1 && PyErr_Occurred())
			return NULL;
	}
	/* Even if offset is 0, don't call timetuple() -- tm_isdst should be
	 * 0 in a UTC timetuple regardless of what dst() says.
	 */
	if (offset) {
		/* Subtract offset minutes & normalize. */
		int stat;

		mm -= offset;
		stat = normalize_datetime(&y, &m, &d, &hh, &mm, &ss, &us);
		if (stat < 0) {
			/* At the edges, it's possible we overflowed
			 * beyond MINYEAR or MAXYEAR.
			 */
			if (PyErr_ExceptionMatches(PyExc_OverflowError))
				PyErr_Clear();
			else
				return NULL;
		}
	}
	return build_struct_time(y, m, d, hh, mm, ss, 0);
}

/* Pickle support, a simple use of __reduce__. */

/* Let basestate be the non-tzinfo data string.
 * If tzinfo is None, this returns (basestate,), else (basestate, tzinfo).
 * So it's a tuple in any (non-error) case.
 * __getstate__ isn't exposed.
 */
static PyObject *
datetime_getstate(PyDateTime_DateTime *self)
{
	PyObject *basestate;
	PyObject *result = NULL;

	basestate = PyString_FromStringAndSize((char *)self->data,
					       _PyDateTime_DATETIME_DATASIZE);
	if (basestate != NULL) {
		if (! HASTZINFO(self) || self->tzinfo == Py_None)
			result = PyTuple_Pack(1, basestate);
		else
			result = PyTuple_Pack(2, basestate, self->tzinfo);
		Py_DECREF(basestate);
	}
	return result;
}

static PyObject *
datetime_reduce(PyDateTime_DateTime *self, PyObject *arg)
{
	return Py_BuildValue("(ON)", Py_TYPE(self), datetime_getstate(self));
}

static PyMethodDef datetime_methods[] = {

	/* Class methods: */

	{"now",         (PyCFunction)datetime_now,
	 METH_VARARGS | METH_KEYWORDS | METH_CLASS,
	 PyDoc_STR("[tz] -> new datetime with tz's local day and time.")},

	{"utcnow",         (PyCFunction)datetime_utcnow,
	 METH_NOARGS | METH_CLASS,
	 PyDoc_STR("Return a new datetime representing UTC day and time.")},

	{"fromtimestamp", (PyCFunction)datetime_fromtimestamp,
	 METH_VARARGS | METH_KEYWORDS | METH_CLASS,
	 PyDoc_STR("timestamp[, tz] -> tz's local time from POSIX timestamp.")},

	{"utcfromtimestamp", (PyCFunction)datetime_utcfromtimestamp,
	 METH_VARARGS | METH_CLASS,
	 PyDoc_STR("timestamp -> UTC datetime from a POSIX timestamp "
	 	   "(like time.time()).")},

	{"strptime", (PyCFunction)datetime_strptime,
	 METH_VARARGS | METH_CLASS,
	 PyDoc_STR("string, format -> new datetime parsed from a string "
	 	   "(like time.strptime()).")},

	{"combine", (PyCFunction)datetime_combine,
	 METH_VARARGS | METH_KEYWORDS | METH_CLASS,
	 PyDoc_STR("date, time -> datetime with same date and time fields")},

	/* Instance methods: */

	{"date",   (PyCFunction)datetime_getdate, METH_NOARGS,
         PyDoc_STR("Return date object with same year, month and day.")},

	{"time",   (PyCFunction)datetime_gettime, METH_NOARGS,
         PyDoc_STR("Return time object with same time but with tzinfo=None.")},

	{"timetz",   (PyCFunction)datetime_gettimetz, METH_NOARGS,
         PyDoc_STR("Return time object with same time and tzinfo.")},

	{"ctime",       (PyCFunction)datetime_ctime,	METH_NOARGS,
	 PyDoc_STR("Return ctime() style string.")},

	{"timetuple",   (PyCFunction)datetime_timetuple, METH_NOARGS,
         PyDoc_STR("Return time tuple, compatible with time.localtime().")},

	{"utctimetuple",   (PyCFunction)datetime_utctimetuple, METH_NOARGS,
         PyDoc_STR("Return UTC time tuple, compatible with time.localtime().")},

	{"isoformat",   (PyCFunction)datetime_isoformat, METH_VARARGS | METH_KEYWORDS,
	 PyDoc_STR("[sep] -> string in ISO 8601 format, "
	 	   "YYYY-MM-DDTHH:MM:SS[.mmmmmm][+HH:MM].\n\n"
	 	   "sep is used to separate the year from the time, and "
	 	   "defaults to 'T'.")},

	{"utcoffset",	(PyCFunction)datetime_utcoffset, METH_NOARGS,
	 PyDoc_STR("Return self.tzinfo.utcoffset(self).")},

	{"tzname",	(PyCFunction)datetime_tzname,	METH_NOARGS,
	 PyDoc_STR("Return self.tzinfo.tzname(self).")},

	{"dst",		(PyCFunction)datetime_dst, METH_NOARGS,
	 PyDoc_STR("Return self.tzinfo.dst(self).")},

	{"replace",     (PyCFunction)datetime_replace,	METH_VARARGS | METH_KEYWORDS,
	 PyDoc_STR("Return datetime with new specified fields.")},

	{"astimezone",  (PyCFunction)datetime_astimezone, METH_VARARGS | METH_KEYWORDS,
	 PyDoc_STR("tz -> convert to local time in new timezone tz\n")},

	{"__reduce__", (PyCFunction)datetime_reduce,     METH_NOARGS,
	 PyDoc_STR("__reduce__() -> (cls, state)")},

	{NULL,	NULL}
};

static char datetime_doc[] =
PyDoc_STR("datetime(year, month, day[, hour[, minute[, second[, microsecond[,tzinfo]]]]])\n\
\n\
The year, month and day arguments are required. tzinfo may be None, or an\n\
instance of a tzinfo subclass. The remaining arguments may be ints or longs.\n");

static PyNumberMethods datetime_as_number = {
	datetime_add,				/* nb_add */
	datetime_subtract,			/* nb_subtract */
	0,					/* nb_multiply */
	0,					/* nb_remainder */
	0,					/* nb_divmod */
	0,					/* nb_power */
	0,					/* nb_negative */
	0,					/* nb_positive */
	0,					/* nb_absolute */
	0,					/* nb_bool */
};

static PyTypeObject PyDateTime_DateTimeType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"datetime.datetime",			/* tp_name */
	sizeof(PyDateTime_DateTime),		/* tp_basicsize */
	0,					/* tp_itemsize */
	(destructor)datetime_dealloc,		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	(reprfunc)datetime_repr,		/* tp_repr */
	&datetime_as_number,			/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	(hashfunc)datetime_hash,		/* tp_hash */
	0,              			/* tp_call */
	(reprfunc)datetime_str,			/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
	datetime_doc,				/* tp_doc */
	0,					/* tp_traverse */
	0,					/* tp_clear */
	datetime_richcompare,			/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	0,					/* tp_iter */
	0,					/* tp_iternext */
	datetime_methods,			/* tp_methods */
	0,					/* tp_members */
	datetime_getset,			/* tp_getset */
	&PyDateTime_DateType,			/* tp_base */
	0,					/* tp_dict */
	0,					/* tp_descr_get */
	0,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	0,					/* tp_init */
	datetime_new,				/* tp_new */
};

/* ---------------------------------------------------------------------------
 * Module methods and initialization.
 */

static PyMethodDef module_methods[] = {
	{NULL, NULL}
};

/* C API.  Clients get at this via PyDateTime_IMPORT, defined in
 * datetime.h.
 */
static PyDateTime_CAPI CAPI = {
        &PyDateTime_DateType,
        &PyDateTime_DateTimeType,
        &PyDateTime_TimeType,
        &PyDateTime_DeltaType,
        &PyDateTime_TZInfoType,
        new_date_ex,
        new_datetime_ex,
        new_time_ex,
        new_delta_ex,
        datetime_fromtimestamp,
        date_fromtimestamp
};


PyMODINIT_FUNC
initdatetime(void)
{
	PyObject *m;	/* a module object */
	PyObject *d;	/* its dict */
	PyObject *x;

	m = Py_InitModule3("datetime", module_methods,
			   "Fast implementation of the datetime type.");
	if (m == NULL)
		return;

	if (PyType_Ready(&PyDateTime_DateType) < 0)
		return;
	if (PyType_Ready(&PyDateTime_DateTimeType) < 0)
		return;
	if (PyType_Ready(&PyDateTime_DeltaType) < 0)
		return;
	if (PyType_Ready(&PyDateTime_TimeType) < 0)
		return;
	if (PyType_Ready(&PyDateTime_TZInfoType) < 0)
		return;

	/* timedelta values */
	d = PyDateTime_DeltaType.tp_dict;

	x = new_delta(0, 0, 1, 0);
	if (x == NULL || PyDict_SetItemString(d, "resolution", x) < 0)
		return;
	Py_DECREF(x);

	x = new_delta(-MAX_DELTA_DAYS, 0, 0, 0);
	if (x == NULL || PyDict_SetItemString(d, "min", x) < 0)
		return;
	Py_DECREF(x);

	x = new_delta(MAX_DELTA_DAYS, 24*3600-1, 1000000-1, 0);
	if (x == NULL || PyDict_SetItemString(d, "max", x) < 0)
		return;
	Py_DECREF(x);

	/* date values */
	d = PyDateTime_DateType.tp_dict;

	x = new_date(1, 1, 1);
	if (x == NULL || PyDict_SetItemString(d, "min", x) < 0)
		return;
	Py_DECREF(x);

	x = new_date(MAXYEAR, 12, 31);
	if (x == NULL || PyDict_SetItemString(d, "max", x) < 0)
		return;
	Py_DECREF(x);

	x = new_delta(1, 0, 0, 0);
	if (x == NULL || PyDict_SetItemString(d, "resolution", x) < 0)
		return;
	Py_DECREF(x);

	/* time values */
	d = PyDateTime_TimeType.tp_dict;

	x = new_time(0, 0, 0, 0, Py_None);
	if (x == NULL || PyDict_SetItemString(d, "min", x) < 0)
		return;
	Py_DECREF(x);

	x = new_time(23, 59, 59, 999999, Py_None);
	if (x == NULL || PyDict_SetItemString(d, "max", x) < 0)
		return;
	Py_DECREF(x);

	x = new_delta(0, 0, 1, 0);
	if (x == NULL || PyDict_SetItemString(d, "resolution", x) < 0)
		return;
	Py_DECREF(x);

	/* datetime values */
	d = PyDateTime_DateTimeType.tp_dict;

	x = new_datetime(1, 1, 1, 0, 0, 0, 0, Py_None);
	if (x == NULL || PyDict_SetItemString(d, "min", x) < 0)
		return;
	Py_DECREF(x);

	x = new_datetime(MAXYEAR, 12, 31, 23, 59, 59, 999999, Py_None);
	if (x == NULL || PyDict_SetItemString(d, "max", x) < 0)
		return;
	Py_DECREF(x);

	x = new_delta(0, 0, 1, 0);
	if (x == NULL || PyDict_SetItemString(d, "resolution", x) < 0)
		return;
	Py_DECREF(x);

	/* module initialization */
	PyModule_AddIntConstant(m, "MINYEAR", MINYEAR);
	PyModule_AddIntConstant(m, "MAXYEAR", MAXYEAR);

	Py_INCREF(&PyDateTime_DateType);
	PyModule_AddObject(m, "date", (PyObject *) &PyDateTime_DateType);

	Py_INCREF(&PyDateTime_DateTimeType);
	PyModule_AddObject(m, "datetime",
			   (PyObject *)&PyDateTime_DateTimeType);

	Py_INCREF(&PyDateTime_TimeType);
	PyModule_AddObject(m, "time", (PyObject *) &PyDateTime_TimeType);

	Py_INCREF(&PyDateTime_DeltaType);
	PyModule_AddObject(m, "timedelta", (PyObject *) &PyDateTime_DeltaType);

	Py_INCREF(&PyDateTime_TZInfoType);
	PyModule_AddObject(m, "tzinfo", (PyObject *) &PyDateTime_TZInfoType);

        x = PyCObject_FromVoidPtrAndDesc(&CAPI, (void*) DATETIME_API_MAGIC,
                NULL);
        if (x == NULL)
            return;
        PyModule_AddObject(m, "datetime_CAPI", x);

	/* A 4-year cycle has an extra leap day over what we'd get from
	 * pasting together 4 single years.
	 */
	assert(DI4Y == 4 * 365 + 1);
	assert(DI4Y == days_before_year(4+1));

	/* Similarly, a 400-year cycle has an extra leap day over what we'd
	 * get from pasting together 4 100-year cycles.
	 */
	assert(DI400Y == 4 * DI100Y + 1);
	assert(DI400Y == days_before_year(400+1));

	/* OTOH, a 100-year cycle has one fewer leap day than we'd get from
	 * pasting together 25 4-year cycles.
	 */
	assert(DI100Y == 25 * DI4Y - 1);
	assert(DI100Y == days_before_year(100+1));

	us_per_us = PyLong_FromLong(1);
	us_per_ms = PyLong_FromLong(1000);
	us_per_second = PyLong_FromLong(1000000);
	us_per_minute = PyLong_FromLong(60000000);
	seconds_per_day = PyLong_FromLong(24 * 3600);
	if (us_per_us == NULL || us_per_ms == NULL || us_per_second == NULL ||
	    us_per_minute == NULL || seconds_per_day == NULL)
		return;

	/* The rest are too big for 32-bit ints, but even
	 * us_per_week fits in 40 bits, so doubles should be exact.
	 */
	us_per_hour = PyLong_FromDouble(3600000000.0);
	us_per_day = PyLong_FromDouble(86400000000.0);
	us_per_week = PyLong_FromDouble(604800000000.0);
	if (us_per_hour == NULL || us_per_day == NULL || us_per_week == NULL)
		return;
}

/* ---------------------------------------------------------------------------
Some time zone algebra.  For a datetime x, let
    x.n = x stripped of its timezone -- its naive time.
    x.o = x.utcoffset(), and assuming that doesn't raise an exception or
          return None
    x.d = x.dst(), and assuming that doesn't raise an exception or
          return None
    x.s = x's standard offset, x.o - x.d

Now some derived rules, where k is a duration (timedelta).

1. x.o = x.s + x.d
   This follows from the definition of x.s.

2. If x and y have the same tzinfo member, x.s = y.s.
   This is actually a requirement, an assumption we need to make about
   sane tzinfo classes.

3. The naive UTC time corresponding to x is x.n - x.o.
   This is again a requirement for a sane tzinfo class.

4. (x+k).s = x.s
   This follows from #2, and that datimetimetz+timedelta preserves tzinfo.

5. (x+k).n = x.n + k
   Again follows from how arithmetic is defined.

Now we can explain tz.fromutc(x).  Let's assume it's an interesting case
(meaning that the various tzinfo methods exist, and don't blow up or return
None when called).

The function wants to return a datetime y with timezone tz, equivalent to x.
x is already in UTC.

By #3, we want

    y.n - y.o = x.n                             [1]

The algorithm starts by attaching tz to x.n, and calling that y.  So
x.n = y.n at the start.  Then it wants to add a duration k to y, so that [1]
becomes true; in effect, we want to solve [2] for k:

   (y+k).n - (y+k).o = x.n                      [2]

By #1, this is the same as

   (y+k).n - ((y+k).s + (y+k).d) = x.n          [3]

By #5, (y+k).n = y.n + k, which equals x.n + k because x.n=y.n at the start.
Substituting that into [3],

   x.n + k - (y+k).s - (y+k).d = x.n; the x.n terms cancel, leaving
   k - (y+k).s - (y+k).d = 0; rearranging,
   k = (y+k).s - (y+k).d; by #4, (y+k).s == y.s, so
   k = y.s - (y+k).d

On the RHS, (y+k).d can't be computed directly, but y.s can be, and we
approximate k by ignoring the (y+k).d term at first.  Note that k can't be
very large, since all offset-returning methods return a duration of magnitude
less than 24 hours.  For that reason, if y is firmly in std time, (y+k).d must
be 0, so ignoring it has no consequence then.

In any case, the new value is

    z = y + y.s                                 [4]

It's helpful to step back at look at [4] from a higher level:  it's simply
mapping from UTC to tz's standard time.

At this point, if

    z.n - z.o = x.n                             [5]

we have an equivalent time, and are almost done.  The insecurity here is
at the start of daylight time.  Picture US Eastern for concreteness.  The wall
time jumps from 1:59 to 3:00, and wall hours of the form 2:MM don't make good
sense then.  The docs ask that an Eastern tzinfo class consider such a time to
be EDT (because it's "after 2"), which is a redundant spelling of 1:MM EST
on the day DST starts.  We want to return the 1:MM EST spelling because that's
the only spelling that makes sense on the local wall clock.

In fact, if [5] holds at this point, we do have the standard-time spelling,
but that takes a bit of proof.  We first prove a stronger result.  What's the
difference between the LHS and RHS of [5]?  Let

    diff = x.n - (z.n - z.o)                    [6]

Now
    z.n =                       by [4]
    (y + y.s).n =               by #5
    y.n + y.s =                 since y.n = x.n
    x.n + y.s =                 since z and y are have the same tzinfo member,
                                    y.s = z.s by #2
    x.n + z.s

Plugging that back into [6] gives

    diff =
    x.n - ((x.n + z.s) - z.o) =     expanding
    x.n - x.n - z.s + z.o =         cancelling
    - z.s + z.o =                   by #2
    z.d

So diff = z.d.

If [5] is true now, diff = 0, so z.d = 0 too, and we have the standard-time
spelling we wanted in the endcase described above.  We're done.  Contrarily,
if z.d = 0, then we have a UTC equivalent, and are also done.

If [5] is not true now, diff = z.d != 0, and z.d is the offset we need to
add to z (in effect, z is in tz's standard time, and we need to shift the
local clock into tz's daylight time).

Let

    z' = z + z.d = z + diff                     [7]

and we can again ask whether

    z'.n - z'.o = x.n                           [8]

If so, we're done.  If not, the tzinfo class is insane, according to the
assumptions we've made.  This also requires a bit of proof.  As before, let's
compute the difference between the LHS and RHS of [8] (and skipping some of
the justifications for the kinds of substitutions we've done several times
already):

    diff' = x.n - (z'.n - z'.o) =           replacing z'.n via [7]
            x.n  - (z.n + diff - z'.o) =    replacing diff via [6]
            x.n - (z.n + x.n - (z.n - z.o) - z'.o) =
            x.n - z.n - x.n + z.n - z.o + z'.o =    cancel x.n
            - z.n + z.n - z.o + z'.o =              cancel z.n
            - z.o + z'.o =                      #1 twice
            -z.s - z.d + z'.s + z'.d =          z and z' have same tzinfo
            z'.d - z.d

So z' is UTC-equivalent to x iff z'.d = z.d at this point.  If they are equal,
we've found the UTC-equivalent so are done.  In fact, we stop with [7] and
return z', not bothering to compute z'.d.

How could z.d and z'd differ?  z' = z + z.d [7], so merely moving z' by
a dst() offset, and starting *from* a time already in DST (we know z.d != 0),
would have to change the result dst() returns:  we start in DST, and moving
a little further into it takes us out of DST.

There isn't a sane case where this can happen.  The closest it gets is at
the end of DST, where there's an hour in UTC with no spelling in a hybrid
tzinfo class.  In US Eastern, that's 5:MM UTC = 0:MM EST = 1:MM EDT.  During
that hour, on an Eastern clock 1:MM is taken as being in standard time (6:MM
UTC) because the docs insist on that, but 0:MM is taken as being in daylight
time (4:MM UTC).  There is no local time mapping to 5:MM UTC.  The local
clock jumps from 1:59 back to 1:00 again, and repeats the 1:MM hour in
standard time.  Since that's what the local clock *does*, we want to map both
UTC hours 5:MM and 6:MM to 1:MM Eastern.  The result is ambiguous
in local time, but so it goes -- it's the way the local clock works.

When x = 5:MM UTC is the input to this algorithm, x.o=0, y.o=-5 and y.d=0,
so z=0:MM.  z.d=60 (minutes) then, so [5] doesn't hold and we keep going.
z' = z + z.d = 1:MM then, and z'.d=0, and z'.d - z.d = -60 != 0 so [8]
(correctly) concludes that z' is not UTC-equivalent to x.

Because we know z.d said z was in daylight time (else [5] would have held and
we would have stopped then), and we know z.d != z'.d (else [8] would have held
and we would have stopped then), and there are only 2 possible values dst() can
return in Eastern, it follows that z'.d must be 0 (which it is in the example,
but the reasoning doesn't depend on the example -- it depends on there being
two possible dst() outcomes, one zero and the other non-zero).  Therefore
z' must be in standard time, and is the spelling we want in this case.

Note again that z' is not UTC-equivalent as far as the hybrid tzinfo class is
concerned (because it takes z' as being in standard time rather than the
daylight time we intend here), but returning it gives the real-life "local
clock repeats an hour" behavior when mapping the "unspellable" UTC hour into
tz.

When the input is 6:MM, z=1:MM and z.d=0, and we stop at once, again with
the 1:MM standard time spelling we want.

So how can this break?  One of the assumptions must be violated.  Two
possibilities:

1) [2] effectively says that y.s is invariant across all y belong to a given
   time zone.  This isn't true if, for political reasons or continental drift,
   a region decides to change its base offset from UTC.

2) There may be versions of "double daylight" time where the tail end of
   the analysis gives up a step too early.  I haven't thought about that
   enough to say.

In any case, it's clear that the default fromutc() is strong enough to handle
"almost all" time zones:  so long as the standard offset is invariant, it
doesn't matter if daylight time transition points change from year to year, or
if daylight time is skipped in some years; it doesn't matter how large or
small dst() may get within its bounds; and it doesn't even matter if some
perverse time zone returns a negative dst()).  So a breaking case must be
pretty bizarre, and a tzinfo subclass can override fromutc() if it is.
--------------------------------------------------------------------------- */
