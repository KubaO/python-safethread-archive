
/* Generic object operations; and implementation of None (NoObject) */

#include "Python.h"
#include "sliceobject.h" /* For PyEllipsis_Type */
#include "monitorobject.h"
#include "branchobject.h"
#include "pythread.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef Py_REF_DEBUG
AO_t _Py_RefTotal;

Py_ssize_t
_Py_GetRefTotal(void)
{
	PyObject *o;
	Py_ssize_t total = AO_load_full(&_Py_RefTotal);
        /* ignore the references to the dummy object of the dicts and sets
           because they are not reliable and not useful (now that the
           hash table code is well-tested) */
	o = _PyDict_Dummy();
	if (o != NULL)
		total -= Py_RefcntSnoop(o);
	o = _PySet_Dummy();
	if (o != NULL)
		total -= Py_RefcntSnoop(o);
	return total;
}
#endif /* Py_REF_DEBUG */

int Py_DivisionWarningFlag;

/* Object allocation routines used by NEWOBJ and NEWVAROBJ macros.
   These are used by the individual routines for object creation.
   Do not call them otherwise, they do not initialize the object! */

#ifdef Py_TRACE_REFS
/* Head of circular doubly-linked list of all objects.  These are linked
 * together via the _ob_prev and _ob_next members of a PyObject, which
 * exist only in a Py_TRACE_REFS build.
 */
static PyObject refchain = {&refchain, &refchain};
static PyThread_type_lock refchain_lock;

/* Insert op at the front of the list of all objects.  If force is true,
 * op is added even if _ob_prev and _ob_next are non-NULL already.  If
 * force is false and _ob_prev or _ob_next are non-NULL, do nothing.
 * force should be true if and only if op points to freshly allocated,
 * uninitialized memory, or you've unlinked op from the list and are
 * relinking it into the front.
 * Note that objects are normally added to the list via _Py_NewReference,
 * which is called by PyObject_Init.  Not all objects are initialized that
 * way, though; exceptions include statically allocated type objects, and
 * statically allocated singletons (like Py_True and Py_None).
 */
void
_Py_AddToAllObjects(PyObject *op, int force)
{
	PyThread_lock_acquire(refchain_lock);
#ifdef  Py_DEBUG
	if (!force) {
		/* If it's initialized memory, op must be in or out of
		 * the list unambiguously.
		 */
		assert((op->_ob_prev == NULL) == (op->_ob_next == NULL));
	}
#endif
	if (force || op->_ob_prev == NULL) {
		op->_ob_next = refchain._ob_next;
		op->_ob_prev = &refchain;
		refchain._ob_next->_ob_prev = op;
		refchain._ob_next = op;
	}
	PyThread_lock_release(refchain_lock);
}
#endif	/* Py_TRACE_REFS */

#ifdef COUNT_ALLOCS
static PyTypeObject *type_list;
/* All types are added to type_list, at least when
   they get one object created. That makes them
   immortal, which unfortunately contributes to
   garbage itself. If unlist_types_without_objects
   is set, they will be removed from the type_list
   once the last object is deallocated. */
int unlist_types_without_objects;
extern int tuple_zero_allocs, fast_tuple_allocs;
extern int quick_int_allocs, quick_neg_int_allocs;
extern int null_strings, one_strings;
void
dump_counts(FILE* f)
{
	PyTypeObject *tp;

	for (tp = type_list; tp; tp = tp->tp_next)
		fprintf(f, "%s alloc'd: %d, freed: %d, max in use: %d\n",
			tp->tp_name, tp->tp_allocs, tp->tp_frees,
			tp->tp_maxalloc);
	fprintf(f, "fast tuple allocs: %d, empty: %d\n",
		fast_tuple_allocs, tuple_zero_allocs);
	fprintf(f, "fast int allocs: pos: %d, neg: %d\n",
		quick_int_allocs, quick_neg_int_allocs);
	fprintf(f, "null strings: %d, 1-strings: %d\n",
		null_strings, one_strings);
}

PyObject *
get_counts(void)
{
	PyTypeObject *tp;
	PyObject *result;
	PyObject *v;

	result = PyList_New(0);
	if (result == NULL)
		return NULL;
	for (tp = type_list; tp; tp = tp->tp_next) {
		v = Py_BuildValue("(snnn)", tp->tp_name, tp->tp_allocs,
				  tp->tp_frees, tp->tp_maxalloc);
		if (v == NULL) {
			Py_DECREF(result);
			return NULL;
		}
		if (PyList_Append(result, v) < 0) {
			Py_DECREF(v);
			Py_DECREF(result);
			return NULL;
		}
		Py_DECREF(v);
	}
	return result;
}

void
inc_count(PyTypeObject *tp)
{
	if (tp->tp_next == NULL && tp->tp_prev == NULL) {
		/* first time; insert in linked list */
		if (tp->tp_next != NULL) /* sanity check */
			Py_FatalError("XXX inc_count sanity check");
		if (type_list)
			type_list->tp_prev = tp;
		tp->tp_next = type_list;
		/* Note that as of Python 2.2, heap-allocated type objects
		 * can go away, but this code requires that they stay alive
		 * until program exit.  That's why we're careful with
		 * refcounts here.  type_list gets a new reference to tp,
		 * while ownership of the reference type_list used to hold
		 * (if any) was transferred to tp->tp_next in the line above.
		 * tp is thus effectively immortal after this.
		 */
		Py_INCREF(tp);
		type_list = tp;
#ifdef Py_TRACE_REFS
		/* Also insert in the doubly-linked list of all objects,
		 * if not already there.
		 */
		_Py_AddToAllObjects((PyObject *)tp, 0);
#endif
	}
	tp->tp_allocs++;
	if (tp->tp_allocs - tp->tp_frees > tp->tp_maxalloc)
		tp->tp_maxalloc = tp->tp_allocs - tp->tp_frees;
}

void dec_count(PyTypeObject *tp)
{
	tp->tp_frees++;
	if (unlist_types_without_objects &&
	    tp->tp_allocs == tp->tp_frees) {
		/* unlink the type from type_list */
		if (tp->tp_prev)
			tp->tp_prev->tp_next = tp->tp_next;
		else
			type_list = tp->tp_next;
		if (tp->tp_next)
			tp->tp_next->tp_prev = tp->tp_prev;
		tp->tp_next = tp->tp_prev = NULL;
		Py_DECREF(tp);
	}
}

#endif

#ifdef Py_REF_DEBUG
/* Log a fatal error; doesn't return. */
void
_Py_NegativeRefcount(const char *fname, int lineno, PyObject *op,
	Py_ssize_t count)
{
	char buf[300];

	PyOS_snprintf(buf, sizeof(buf),
		      "%s:%i object at %p has negative ref count "
		      "%" PY_FORMAT_SIZE_T "d",
		      fname, lineno, op, count);
	Py_FatalError(buf);
}

#endif /* Py_REF_DEBUG */


/* Implementation of PyObject_Print with recursion checking */
static int
internal_print(PyObject *op, FILE *fp, int flags, int nesting)
{
	int ret = 0;
	if (nesting > 10) {
		PyErr_SetString(PyExc_RuntimeError, "print recursion");
		return -1;
	}
#ifdef USE_STACKCHECK
	if (PyOS_CheckStack()) {
		PyErr_SetString(PyExc_MemoryError, "stack overflow");
		return -1;
	}
#endif
	clearerr(fp); /* Clear any previous error condition */
	if (op == NULL) {
		Py_BEGIN_ALLOW_THREADS
		fprintf(fp, "<nil>");
		Py_END_ALLOW_THREADS
	}
	else {
		if (Py_RefcntSnoop(op) <= 0) {
			/* XXX(twouters) cast refcount to long until %zd is
			   universally available */
			Py_BEGIN_ALLOW_THREADS
			fprintf(fp, "<refcnt %ld at %p>",
				(long)Py_RefcntSnoop(op), op);
			Py_END_ALLOW_THREADS
		} else {
			PyObject *s;
			if (flags & Py_PRINT_RAW)
				s = PyObject_Str(op);
			else
				s = PyObject_Repr(op);
			if (s == NULL)
				ret = -1;
                        else if (PyString_Check(s)) {
				fwrite(PyString_AS_STRING(s), 1,
				       PyString_GET_SIZE(s), fp);
			}
			else if (PyUnicode_Check(s)) {
				PyObject *t;
				t = _PyUnicode_AsDefaultEncodedString(s, NULL);
				if (t == NULL)
					ret = 0;
				else {
					fwrite(PyString_AS_STRING(t), 1,
					       PyString_GET_SIZE(t), fp);
				}
			}
			else {
				PyErr_Format(PyExc_TypeError,
					     "str() or repr() returned '%.100s'",
					     s->ob_type->tp_name);
				ret = -1;
			}
			Py_XDECREF(s);
		}
	}
	if (ret == 0) {
		if (ferror(fp)) {
			PyErr_SetFromErrno(PyExc_IOError);
			clearerr(fp);
			ret = -1;
		}
	}
	return ret;
}

int
PyObject_Print(PyObject *op, FILE *fp, int flags)
{
	return internal_print(op, fp, flags, 0);
}

/* For debugging convenience.  Set a breakpoint here and call it from your DLL */
void
_Py_BreakPoint(void)
{
}


/* For debugging convenience.  See Misc/gdbinit for some useful gdb hooks */
void
_PyObject_Dump(PyObject* op)
{
	if (op == NULL)
		fprintf(stderr, "NULL\n");
	else {
		fprintf(stderr, "object  : ");
		//(void)PyObject_Print(op, stderr, 0);
		/* XXX(twouters) cast refcount to long until %zd is
		   universally available */
		fprintf(stderr, "\n"
			"type    : %s\n"
			"refcount: %ld\n"
			"address : %p\n",
			Py_Type(op)==NULL ? "NULL" : Py_Type(op)->tp_name,
			(long)Py_RefcntSnoop(op),
			op);
	}
}

PyObject *
PyObject_Repr(PyObject *v)
{
	PyObject *res;
#ifdef USE_STACKCHECK
	if (PyOS_CheckStack()) {
		PyErr_SetString(PyExc_MemoryError, "stack overflow");
		return NULL;
	}
#endif
	if (v == NULL)
		return PyUnicode_FromString("<NULL>");
	else if (Py_Type(v)->tp_repr == NULL)
		return PyUnicode_FromFormat("<%s object at %p>", v->ob_type->tp_name, v);
	else {
		res = (*v->ob_type->tp_repr)(v);
		if (res != NULL && !PyUnicode_Check(res)) {
			PyErr_Format(PyExc_TypeError,
				     "__repr__ returned non-string (type %.200s)",
				     res->ob_type->tp_name);
			Py_DECREF(res);
			return NULL;
		}
		return res;
	}
}

PyObject *
PyObject_ReprStr8(PyObject *v)
{
	PyObject *resu = PyObject_Repr(v);
	if (resu) {
		PyObject *resb = PyUnicode_AsEncodedString(resu, NULL, NULL);
		Py_DECREF(resu);
		if (resb) {
			PyObject *ress = PyString_FromStringAndSize(
				PyBytes_AS_STRING(resb),
				PyBytes_GET_SIZE(resb)
			);
			Py_DECREF(resb);
			return ress;
		}
	}
	return NULL;
}

PyObject *
_PyObject_Str(PyObject *v)
{
	PyObject *res;
	if (v == NULL)
		return PyUnicode_FromString("<NULL>");
	if (PyString_CheckExact(v)) {
		Py_INCREF(v);
		return v;
	}
	if (PyUnicode_CheckExact(v)) {
		Py_INCREF(v);
		return v;
	}
	if (Py_Type(v)->tp_str == NULL)
		return PyObject_Repr(v);

	res = (*Py_Type(v)->tp_str)(v);
	if (res == NULL)
		return NULL;
	if (!(PyString_Check(res) || PyUnicode_Check(res))) {
		PyErr_Format(PyExc_TypeError,
			     "__str__ returned non-string (type %.200s)",
			     Py_Type(res)->tp_name);
		Py_DECREF(res);
		return NULL;
	}
	return res;
}

PyObject *
PyObject_Str(PyObject *v)
{
	PyObject *res = _PyObject_Str(v);
	if (res == NULL)
		return NULL;
	if (PyUnicode_Check(res)) {
		PyObject* str;
		str = _PyUnicode_AsDefaultEncodedString(res, NULL);
		Py_XINCREF(str);
		Py_DECREF(res);
		if (str)
			res = str;
		else
		    	return NULL;
	}
	assert(PyString_Check(res));
	return res;
}

PyObject *
PyObject_Unicode(PyObject *v)
{
	PyObject *res;
	PyObject *func;
	PyObject *str;
	static PyObject *unicodestr;

	if (v == NULL)
		return PyUnicode_FromString("<NULL>");
	else if (PyUnicode_CheckExact(v)) {
		Py_INCREF(v);
		return v;
	}
	/* XXX As soon as we have a tp_unicode slot, we should
	   check this before trying the __unicode__
	   method. */
	if (unicodestr == NULL) {
		unicodestr= PyUnicode_InternFromString("__unicode__");
		if (unicodestr == NULL)
			return NULL;
	}
	func = PyObject_GetAttr(v, unicodestr);
	if (func != NULL) {
		res = PyEval_CallObject(func, (PyObject *)NULL);
		Py_DECREF(func);
	}
	else {
		PyErr_Clear();
		if (PyUnicode_Check(v) &&
		    v->ob_type->tp_str == PyUnicode_Type.tp_str) {
			/* For a Unicode subtype that's didn't overwrite
			   __unicode__ or __str__,
			   return a true Unicode object with the same data. */
			return PyUnicode_FromUnicode(PyUnicode_AS_UNICODE(v),
			                             PyUnicode_GET_SIZE(v));
		}
		if (PyString_CheckExact(v)) {
			Py_INCREF(v);
			res = v;
		}
		else {
			if (Py_Type(v)->tp_str != NULL)
				res = (*Py_Type(v)->tp_str)(v);
			else
				res = PyObject_Repr(v);
		}
	}
	if (res == NULL)
		return NULL;
	if (!PyUnicode_Check(res)) {
		str = PyUnicode_FromEncodedObject(res, NULL, "strict");
		Py_DECREF(res);
		res = str;
	}
	return res;
}


/* The new comparison philosophy is: we completely separate three-way
   comparison from rich comparison.  That is, PyObject_Compare() and
   PyObject_Cmp() *just* use the tp_compare slot.  And PyObject_RichCompare()
   and PyObject_RichCompareBool() *just* use the tp_richcompare slot.
   
   See (*) below for practical amendments.

   IOW, only cmp() uses tp_compare; the comparison operators (==, !=, <=, <,
   >=, >) only use tp_richcompare.  Note that list.sort() only uses <.

   (And yes, eventually we'll rip out cmp() and tp_compare.)

   The calling conventions are different: tp_compare only gets called with two
   objects of the appropriate type; tp_richcompare gets called with a first
   argument of the appropriate type and a second object of an arbitrary type.
   We never do any kind of coercion.

   The return conventions are also different.

   The tp_compare slot should return a C int, as follows:

     -1 if a < b or if an exception occurred
      0 if a == b
     +1 if a > b

   No other return values are allowed.  PyObject_Compare() has the same
   calling convention.

  The tp_richcompare slot should return an object, as follows:

    NULL if an exception occurred
    NotImplemented if the requested comparison is not implemented
    any other false value if the requested comparison is false
    any other true value if the requested comparison is true

  The PyObject_RichCompare[Bool]() wrappers raise TypeError when they get
  NotImplemented.

  (*) Practical amendments:

  - If rich comparison returns NotImplemented, == and != are decided by
    comparing the object pointer (i.e. falling back to the base object
    implementation).

  - If three-way comparison is not implemented, it falls back on rich
    comparison (but not the other way around!).

*/

/* Forward */
static PyObject *do_richcompare(PyObject *v, PyObject *w, int op);

/* Perform a three-way comparison, raising TypeError if three-way comparison
   is not supported.  */
static int
do_compare(PyObject *v, PyObject *w)
{
	cmpfunc f;
	int ok;

	if (v->ob_type == w->ob_type && 
	    (f = v->ob_type->tp_compare) != NULL) {
		return (*f)(v, w);
	}

	/* Now try three-way compare before giving up.  This is intentionally
	   elaborate; if you have a it will raise TypeError if it detects two
	   objects that aren't ordered with respect to each other. */
	ok = PyObject_RichCompareBool(v, w, Py_LT);
	if (ok < 0)
		return -1; /* Error */
	if (ok)
		return -1; /* Less than */
	ok = PyObject_RichCompareBool(v, w, Py_GT);
	if (ok < 0)
		return -1; /* Error */
	if (ok)
		return 1; /* Greater than */
	ok = PyObject_RichCompareBool(v, w, Py_EQ);
	if (ok < 0)
		return -1; /* Error */
	if (ok)
		return 0; /* Equal */

	/* Give up */
	PyErr_Format(PyExc_TypeError,
		     "unorderable types: '%.100s' != '%.100s'",
		     v->ob_type->tp_name,
		     w->ob_type->tp_name);
	return -1;
}

/* Perform a three-way comparison.  This wraps do_compare() with a check for
   NULL arguments and a recursion check. */
int
PyObject_Compare(PyObject *v, PyObject *w)
{
	int res;

	if (v == NULL || w == NULL) {
		if (!PyErr_Occurred())
			PyErr_BadInternalCall();
		return -1;
	}
	if (Py_EnterRecursiveCall(" in cmp"))
		return -1;
	res = do_compare(v, w);
	Py_LeaveRecursiveCall();
	return res < 0 ? -1 : res;
}

/* Map rich comparison operators to their swapped version, e.g. LT <--> GT */
int _Py_SwappedOp[] = {Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE};

static char *opstrings[] = {"<", "<=", "==", "!=", ">", ">="};

/* Perform a rich comparison, raising TypeError when the requested comparison
   operator is not supported. */
static PyObject *
do_richcompare(PyObject *v, PyObject *w, int op)
{
	richcmpfunc f;
	PyObject *res;

	if (v->ob_type != w->ob_type &&
	    PyType_IsSubtype(w->ob_type, v->ob_type) &&
	    (f = w->ob_type->tp_richcompare) != NULL) {
		res = (*f)(w, v, _Py_SwappedOp[op]);
		if (res != Py_NotImplemented)
			return res;
		Py_DECREF(res);
	}
	if ((f = v->ob_type->tp_richcompare) != NULL) {
		res = (*f)(v, w, op);
		if (res != Py_NotImplemented)
			return res;
		Py_DECREF(res);
	}
	if ((f = w->ob_type->tp_richcompare) != NULL) {
		res = (*f)(w, v, _Py_SwappedOp[op]);
		if (res != Py_NotImplemented)
			return res;
		Py_DECREF(res);
	}
	/* If neither object implements it, provide a sensible default
	   for == and !=, but raise an exception for ordering. */
	switch (op) {
	case Py_EQ:
		res = (v == w) ? Py_True : Py_False;
		break;
	case Py_NE:
		res = (v != w) ? Py_True : Py_False;
		break;
	default:
		/* XXX Special-case None so it doesn't show as NoneType() */
		PyErr_Format(PyExc_TypeError,
			     "unorderable types: %.100s() %s %.100s()",
			     v->ob_type->tp_name,
			     opstrings[op],
			     w->ob_type->tp_name);
		return NULL;
	}
	Py_INCREF(res);
	return res;
}

/* Perform a rich comparison with object result.  This wraps do_richcompare()
   with a check for NULL arguments and a recursion check. */

PyObject *
PyObject_RichCompare(PyObject *v, PyObject *w, int op)
{
	PyObject *res;

	assert(Py_LT <= op && op <= Py_GE);
	if (v == NULL || w == NULL) {
		if (!PyErr_Occurred())
			PyErr_BadInternalCall();
		return NULL;
	}
	if (Py_EnterRecursiveCall(" in cmp"))
		return NULL;
	res = do_richcompare(v, w, op);
	Py_LeaveRecursiveCall();
	return res;
}

/* Perform a rich comparison with integer result.  This wraps
   PyObject_RichCompare(), returning -1 for error, 0 for false, 1 for true. */
int
PyObject_RichCompareBool(PyObject *v, PyObject *w, int op)
{
	PyObject *res;
	int ok;

	res = PyObject_RichCompare(v, w, op);
	if (res == NULL)
		return -1;
	if (PyBool_Check(res))
		ok = (res == Py_True);
	else
		ok = PyObject_IsTrue(res);
	Py_DECREF(res);
	return ok;
}

/* Turn the result of a three-way comparison into the result expected by a
   rich comparison. */
PyObject *
Py_CmpToRich(int op, int cmp)
{
	PyObject *res;
	int ok;

	if (PyErr_Occurred())
		return NULL;
	switch (op) {
	case Py_LT:
		ok = cmp <  0; 
		break;
	case Py_LE:
		ok = cmp <= 0; 
		break;
	case Py_EQ:
		ok = cmp == 0; 
		break;
	case Py_NE:
		ok = cmp != 0; 
		break;
	case Py_GT: 
		ok = cmp >  0; 
		break;
	case Py_GE:
		ok = cmp >= 0; 
		break;
	default:
		PyErr_BadArgument(); 
		return NULL;
	}
	res = ok ? Py_True : Py_False;
	Py_INCREF(res);
	return res;
}

/* Set of hash utility functions to help maintaining the invariant that
	if a==b then hash(a)==hash(b)

   All the utility functions (_Py_Hash*()) return "-1" to signify an error.
*/

long
_Py_HashDouble(double v)
{
	double intpart, fractpart;
	int expo;
	long hipart;
	long x;		/* the final hash value */
	/* This is designed so that Python numbers of different types
	 * that compare equal hash to the same value; otherwise comparisons
	 * of mapping keys will turn out weird.
	 */

	fractpart = modf(v, &intpart);
	if (fractpart == 0.0) {
		/* This must return the same hash as an equal int or long. */
		if (intpart > LONG_MAX || -intpart > LONG_MAX) {
			/* Convert to long and use its hash. */
			PyObject *plong;	/* converted to Python long */
			if (Py_IS_INFINITY(intpart))
				/* can't convert to long int -- arbitrary */
				v = v < 0 ? -271828.0 : 314159.0;
			plong = PyLong_FromDouble(v);
			if (plong == NULL)
				return -1;
			x = PyObject_Hash(plong);
			Py_DECREF(plong);
			return x;
		}
		/* Fits in a C long == a Python int, so is its own hash. */
		x = (long)intpart;
		if (x == -1)
			x = -2;
		return x;
	}
	/* The fractional part is non-zero, so we don't have to worry about
	 * making this match the hash of some other type.
	 * Use frexp to get at the bits in the double.
	 * Since the VAX D double format has 56 mantissa bits, which is the
	 * most of any double format in use, each of these parts may have as
	 * many as (but no more than) 56 significant bits.
	 * So, assuming sizeof(long) >= 4, each part can be broken into two
	 * longs; frexp and multiplication are used to do that.
	 * Also, since the Cray double format has 15 exponent bits, which is
	 * the most of any double format in use, shifting the exponent field
	 * left by 15 won't overflow a long (again assuming sizeof(long) >= 4).
	 */
	v = frexp(v, &expo);
	v *= 2147483648.0;	/* 2**31 */
	hipart = (long)v;	/* take the top 32 bits */
	v = (v - (double)hipart) * 2147483648.0; /* get the next 32 bits */
	x = hipart + (long)v + (expo << 15);
	if (x == -1)
		x = -2;
	return x;
}

long
_Py_HashPointer(void *p)
{
#if SIZEOF_LONG >= SIZEOF_VOID_P
	return (long)p;
#else
	/* convert to a Python long and hash that */
	PyObject* longobj;
	long x;

	if ((longobj = PyLong_FromVoidPtr(p)) == NULL) {
		x = -1;
		goto finally;
	}
	x = PyObject_Hash(longobj);

finally:
	Py_XDECREF(longobj);
	return x;
#endif
}


long
PyObject_Hash(PyObject *v)
{
	PyTypeObject *tp = v->ob_type;
	if (tp->tp_hash != NULL)
		return (*tp->tp_hash)(v);
	/* Otherwise, the object can't be hashed */
	PyErr_Format(PyExc_TypeError, "unhashable type: '%.200s'",
		     v->ob_type->tp_name);
	return -1;
}

PyObject *
PyObject_GetAttrString(PyObject *v, const char *name)
{
	PyObject *w, *res;

	if (Py_Type(v)->tp_getattr != NULL)
		return (*Py_Type(v)->tp_getattr)(v, (char*)name);
	w = PyUnicode_InternFromString(name);
	if (w == NULL)
		return NULL;
	res = PyObject_GetAttr(v, w);
	Py_XDECREF(w);
	return res;
}

int
PyObject_HasAttrString(PyObject *v, const char *name)
{
	PyObject *res = PyObject_GetAttrString(v, name);
	if (res != NULL) {
		Py_DECREF(res);
		return 1;
	}
	PyErr_Clear();
	return 0;
}

int
PyObject_SetAttrString(PyObject *v, const char *name, PyObject *w)
{
	PyObject *s;
	int res;

	if (Py_Type(v)->tp_setattr != NULL)
		return (*Py_Type(v)->tp_setattr)(v, (char*)name, w);
	s = PyUnicode_InternFromString(name);
	if (s == NULL)
		return -1;
	res = PyObject_SetAttr(v, s, w);
	Py_XDECREF(s);
	return res;
}

PyObject *
PyObject_GetAttr(PyObject *v, PyObject *name)
{
	PyTypeObject *tp = Py_Type(v);

 	if (!PyUnicode_Check(name)) {
		PyErr_Format(PyExc_TypeError,
			     "attribute name must be string, not '%.200s'",
			     name->ob_type->tp_name);
		return NULL;
	}
	if (tp->tp_getattro != NULL)
		return (*tp->tp_getattro)(v, name);
	if (tp->tp_getattr != NULL)
		return (*tp->tp_getattr)(v, PyUnicode_AsString(name));
	PyErr_Format(PyExc_AttributeError,
		     "'%.50s' object has no attribute '%U'",
		     tp->tp_name, name);
	return NULL;
}

int
PyObject_HasAttr(PyObject *v, PyObject *name)
{
	PyObject *res = PyObject_GetAttr(v, name);
	if (res != NULL) {
		Py_DECREF(res);
		return 1;
	}
	PyErr_Clear();
	return 0;
}

int
PyObject_SetAttr(PyObject *v, PyObject *name, PyObject *value)
{
	PyTypeObject *tp = Py_Type(v);
	int err;

	if (!PyUnicode_Check(name)) {
		PyErr_Format(PyExc_TypeError,
			     "attribute name must be string, not '%.200s'",
			     name->ob_type->tp_name);
		return -1;
	}
	Py_INCREF(name);

	PyUnicode_InternInPlace(&name);
	if (tp->tp_setattro != NULL) {
		err = (*tp->tp_setattro)(v, name, value);
		Py_DECREF(name);
		return err;
	}
	if (tp->tp_setattr != NULL) {
		err = (*tp->tp_setattr)(v, PyUnicode_AsString(name), value);
		Py_DECREF(name);
		return err;
	}
	Py_DECREF(name);
	assert(Py_RefcntSnoop(name) >= 1);
	if (tp->tp_getattr == NULL && tp->tp_getattro == NULL)
		PyErr_Format(PyExc_TypeError,
			     "'%.100s' object has no attributes "
			     "(%s .%U)",
			     tp->tp_name,
			     value==NULL ? "del" : "assign to",
			     name);
	else
		PyErr_Format(PyExc_TypeError,
			     "'%.100s' object has only read-only attributes "
			     "(%s .%U)",
			     tp->tp_name,
			     value==NULL ? "del" : "assign to",
			     name);
	return -1;
}

/* Helper to get a pointer to an object's __dict__ slot, if any */

PyObject **
_PyObject_GetDictPtr(PyObject *obj)
{
	if (PyMonitor_Check(obj) &&
			!PyMonitorSpace_IsCurrent(PyMonitor_GetMonitorSpace(obj))) {
		return NULL;
	}

	return _PyObject_GetDictPtr_NoCheck(obj);
}

PyObject **
_PyObject_GetDictPtr_NoCheck(PyObject *obj)
{
	Py_ssize_t dictoffset;
	PyTypeObject *tp = Py_Type(obj);

	dictoffset = tp->tp_dictoffset;
	if (dictoffset == 0)
		return NULL;
	if (dictoffset < 0) {
		Py_ssize_t tsize;
		size_t size;

		tsize = ((PyVarObject *)obj)->ob_size;
		if (tsize < 0)
			tsize = -tsize;
		size = _PyObject_VAR_SIZE(tp, tsize);

		dictoffset += (long)size;
		assert(dictoffset > 0);
		assert(dictoffset % SIZEOF_VOID_P == 0);
	}
	return (PyObject **) ((char *)obj + dictoffset);
}

PyObject *
PyObject_SelfIter(PyObject *obj)
{
	Py_INCREF(obj);
	return obj;
}

/* Generic GetAttr functions - put these in your tp_[gs]etattro slot */

PyObject *
PyObject_GenericGetAttr(PyObject *obj, PyObject *name)
{
	PyThreadState *tstate = PyThreadState_Get();
	PyTypeObject *tp = Py_Type(obj);
	PyObject *descr = NULL;
	PyObject *res = NULL;
	descrgetfunc f;
	Py_ssize_t dictoffset;
	PyObject **dictptr;

	if (!PyUnicode_Check(name)){
		PyErr_Format(PyExc_TypeError,
			     "attribute name must be string, not '%.200s'",
			     name->ob_type->tp_name);
		return NULL;
	}
	else
		Py_INCREFTS(name);

	if (tp->tp_dict == NULL) {
		if (PyType_Ready(tp) < 0)
			goto done;
	}

	/* Inline _PyType_LookupEx */
	{
		Py_ssize_t i, n;
		PyObject *mro, *base, *dict;

		/* Look in tp_dict of types in MRO */
		mro = tp->tp_mro;
		assert(mro != NULL);
		assert(PyTuple_Check(mro));
		n = PyTuple_GET_SIZE(mro);
		for (i = 0; i < n; i++) {
			base = PyTuple_GET_ITEM(mro, i);
			assert(PyType_Check(base));
			dict = ((PyTypeObject *)base)->tp_dict;
			assert(dict && PyDict_Check(dict));
			if (PyDict_GetItemEx(dict, name, &descr) < 0)
				goto done;
			if (descr != NULL)
				break;
		}
	}

	f = NULL;
	if (descr != NULL) {
		f = descr->ob_type->tp_descr_get;
		if (f != NULL && PyDescr_IsData(descr)) {
			res = f(descr, obj, (PyObject *)obj->ob_type);
			Py_DECREFTS(descr);
			goto done;
		}
	}

	/* Inline _PyObject_GetDictPtr */
	dictoffset = tp->tp_dictoffset;
	if (dictoffset != 0 && !(PyMonitor_Check(obj) &&
			!PyMonitorSpace_IsCurrent(PyMonitor_GetMonitorSpace(obj)))) {
		PyObject *dict;
		if (dictoffset < 0) {
			Py_ssize_t tsize;
			size_t size;

			tsize = ((PyVarObject *)obj)->ob_size;
			if (tsize < 0)
				tsize = -tsize;
			size = _PyObject_VAR_SIZE(tp, tsize);

			dictoffset += (long)size;
			assert(dictoffset > 0);
			assert(dictoffset % SIZEOF_VOID_P == 0);
		}
		dictptr = (PyObject **) ((char *)obj + dictoffset);
		dict = *dictptr;
		if (dict != NULL) {
			if (PyDict_GetItemEx(dict, name, &res) <= 0) {
				Py_XDECREFTS(descr);
				goto done;
			}
		}
	}

	if (f != NULL) {
		res = f(descr, obj, (PyObject *)Py_Type(obj));
		Py_DECREFTS(descr);
		goto done;
	}

	if (descr != NULL) {
		res = descr;
		/* descr was already increfed above */
		goto done;
	}

	PyErr_Format(PyExc_AttributeError,
		     "'%.50s' object has no attribute '%.400s'",
		     tp->tp_name, PyUnicode_AsString(name));
  done:
	Py_DECREFTS(name);
	return res;
}

int
PyObject_GenericSetAttr(PyObject *obj, PyObject *name, PyObject *value)
{
	PyTypeObject *tp = Py_Type(obj);
	PyObject *descr = NULL;
	descrsetfunc f;
	PyObject **dictptr;
	int res = -1;

	if (!PyUnicode_Check(name)){
		PyErr_Format(PyExc_TypeError,
			     "attribute name must be string, not '%.200s'",
			     name->ob_type->tp_name);
		return -1;
	}
	else
		Py_INCREF(name);

	if (tp->tp_dict == NULL) {
		if (PyType_Ready(tp) < 0)
			goto done;
	}

	if (_PyType_LookupEx(tp, name, &descr) < 0)
		goto done;
	f = NULL;
	if (descr != NULL) {
		f = descr->ob_type->tp_descr_set;
		if (f != NULL && PyDescr_IsData(descr)) {
			res = f(descr, obj, value);
			goto done;
		}
	}

	dictptr = _PyObject_GetDictPtr(obj);
	if (dictptr != NULL) {
		PyObject *dict = *dictptr;
		if (dict == NULL && value != NULL) {
			dict = PyDict_New();
			if (dict == NULL)
				goto done;
			*dictptr = dict;
		}
		if (dict != NULL) {
			if (value == NULL)
				res = PyDict_DelItem(dict, name);
			else
				res = PyDict_SetItem(dict, name, value);
			if (res < 0 && PyErr_ExceptionMatches(PyExc_KeyError))
				PyErr_SetObject(PyExc_AttributeError, name);
			goto done;
		}
	}

	if (f != NULL) {
		res = f(descr, obj, value);
		goto done;
	}

	if (descr == NULL) {
		PyErr_Format(PyExc_AttributeError,
			     "'%.100s' object has no attribute '%U'",
			     tp->tp_name, name);
		goto done;
	}

	PyErr_Format(PyExc_AttributeError,
		     "'%.50s' object attribute '%U' is read-only",
		     tp->tp_name, name);
  done:
	Py_DECREF(name);
	Py_XDECREF(descr);
	return res;
}

/* Test a value used as condition, e.g., in a for or if statement.
   Return -1 if an error occurred */

int
PyObject_IsTrue(PyObject *v)
{
	Py_ssize_t res;
	if (v == Py_True)
		return 1;
	if (v == Py_False)
		return 0;
	if (v == Py_None)
		return 0;
	else if (v->ob_type->tp_as_number != NULL &&
		 v->ob_type->tp_as_number->nb_bool != NULL)
		res = (*v->ob_type->tp_as_number->nb_bool)(v);
	else if (v->ob_type->tp_as_mapping != NULL &&
		 v->ob_type->tp_as_mapping->mp_length != NULL)
		res = (*v->ob_type->tp_as_mapping->mp_length)(v);
	else if (v->ob_type->tp_as_sequence != NULL &&
		 v->ob_type->tp_as_sequence->sq_length != NULL)
		res = (*v->ob_type->tp_as_sequence->sq_length)(v);
	else
		return 1;
	/* if it is negative, it should be either -1 or -2 */
	return (res > 0) ? 1 : Py_SAFE_DOWNCAST(res, Py_ssize_t, int);
}

/* equivalent of 'not v'
   Return -1 if an error occurred */

int
PyObject_Not(PyObject *v)
{
	int res;
	res = PyObject_IsTrue(v);
	if (res < 0)
		return res;
	return res == 0;
}

/* Test whether an object can be called */

int
PyCallable_Check(PyObject *x)
{
	if (x == NULL)
		return 0;
	return x->ob_type->tp_call != NULL;
}

/* Test whether an object is shareable */

int
PyObject_IsShareable(PyObject *x)
{
	if (x->ob_type->tp_isshareable != NULL)
		return x->ob_type->tp_isshareable(x);
	return 0;
}

/* ------------------------- PyObject_Dir() helpers ------------------------- */

/* Helper for PyObject_Dir.
   Merge the __dict__ of aclass into dict, and recursively also all
   the __dict__s of aclass's base classes.  The order of merging isn't
   defined, as it's expected that only the final set of dict keys is
   interesting.
   Return 0 on success, -1 on error.
*/

static int
merge_class_dict(PyObject* dict, PyObject* aclass)
{
	PyObject *classdict;
	PyObject *bases;

	assert(PyDict_Check(dict));
	assert(aclass);

	/* Merge in the type's dict (if any). */
	classdict = PyObject_GetAttrString(aclass, "__dict__");
	if (classdict == NULL)
		PyErr_Clear();
	else {
		int status = PyDict_Update(dict, classdict);
		Py_DECREF(classdict);
		if (status < 0)
			return -1;
	}

	/* Recursively merge in the base types' (if any) dicts. */
	bases = PyObject_GetAttrString(aclass, "__bases__");
	if (bases == NULL)
		PyErr_Clear();
	else {
		/* We have no guarantee that bases is a real tuple */
		Py_ssize_t i, n;
		n = PySequence_Size(bases); /* This better be right */
		if (n < 0)
			PyErr_Clear();
		else {
			for (i = 0; i < n; i++) {
				int status;
				PyObject *base = PySequence_GetItem(bases, i);
				if (base == NULL) {
					Py_DECREF(bases);
					return -1;
				}
				status = merge_class_dict(dict, base);
				Py_DECREF(base);
				if (status < 0) {
					Py_DECREF(bases);
					return -1;
				}
			}
		}
		Py_DECREF(bases);
	}
	return 0;
}

/* Helper for PyObject_Dir without arguments: returns the local scope. */
static PyObject *
_dir_locals(void)
{
	PyObject *names;
	PyObject *locals = PyEval_GetLocals();

	if (locals == NULL) {
		PyErr_SetString(PyExc_SystemError, "frame does not exist");
		return NULL;
	}

	names = PyMapping_Keys(locals);
	if (!names)
		return NULL;
	if (!PyList_Check(names)) {
		PyErr_Format(PyExc_TypeError,
			"dir(): expected keys() of locals to be a list, "
			"not '%.200s'", Py_Type(names)->tp_name);
		Py_DECREF(names);
		return NULL;
	}
	/* the locals don't need to be DECREF'd */
	return names;
}

/* Helper for PyObject_Dir of type objects: returns __dict__ and __bases__.
   We deliberately don't suck up its __class__, as methods belonging to the 
   metaclass would probably be more confusing than helpful. 
*/
static PyObject * 
_specialized_dir_type(PyObject *obj)
{
	PyObject *result = NULL;
	PyObject *dict = PyDict_New();

	if (dict != NULL && merge_class_dict(dict, obj) == 0)
		result = PyDict_Keys(dict);

	Py_XDECREF(dict);
	return result;
}

/* Helper for PyObject_Dir of module objects: returns the module's __dict__. */
static PyObject *
_specialized_dir_module(PyObject *obj)
{
	PyObject *result = NULL;
	PyObject *dict = PyObject_GetAttrString(obj, "__dict__");

	if (dict != NULL) {
		if (PyDict_Check(dict))
			result = PyDict_Keys(dict);
		else {
			PyErr_Format(PyExc_TypeError,
				     "%.200s.__dict__ is not a dictionary",
				     PyModule_GetName(obj));
		}
	}

	Py_XDECREF(dict);
	return result;
}

/* Helper for PyObject_Dir of generic objects: returns __dict__, __class__,
   and recursively up the __class__.__bases__ chain.
*/
static PyObject *
_generic_dir(PyObject *obj)
{
	PyObject *result = NULL;
	PyObject *dict = NULL;
	PyObject *itsclass = NULL;
	
	/* Get __dict__ (which may or may not be a real dict...) */
	dict = PyObject_GetAttrString(obj, "__dict__");
	if (dict == NULL) {
		PyErr_Clear();
		dict = PyDict_New();
	}
	else if (!PyDict_Check(dict)) {
		Py_DECREF(dict);
		dict = PyDict_New();
	}
	else {
		/* Copy __dict__ to avoid mutating it. */
		PyObject *temp = PyDict_Copy(dict);
		Py_DECREF(dict);
		dict = temp;
	}

	if (dict == NULL)
		goto error;

	/* Merge in attrs reachable from its class. */
	itsclass = PyObject_GetAttrString(obj, "__class__");
	if (itsclass == NULL)
		/* XXX(tomer): Perhaps fall back to obj->ob_type if no
		               __class__ exists? */
		PyErr_Clear();
	else {
		if (merge_class_dict(dict, itsclass) != 0)
			goto error;
	}

	result = PyDict_Keys(dict);
	/* fall through */
error:
	Py_XDECREF(itsclass);
	Py_XDECREF(dict);
	return result;
}

/* Helper for PyObject_Dir: object introspection.
   This calls one of the above specialized versions if no __dir__ method
   exists. */
static PyObject *
_dir_object(PyObject *obj)
{
	PyObject * result = NULL;
	PyObject * dirfunc = PyObject_GetAttrString((PyObject*)obj->ob_type,
						    "__dir__");

	assert(obj);
	if (dirfunc == NULL) {
		/* use default implementation */
		PyErr_Clear();
		if (PyModule_Check(obj))
			result = _specialized_dir_module(obj);
		else if (PyType_Check(obj))
			result = _specialized_dir_type(obj);
		else
			result = _generic_dir(obj);
	}
	else {
		/* use __dir__ */
		result = PyObject_CallFunctionObjArgs(dirfunc, obj, NULL);
		Py_DECREF(dirfunc);
		if (result == NULL)
			return NULL;

		/* result must be a list */
		/* XXX(gbrandl): could also check if all items are strings */
		if (!PyList_Check(result)) {
			PyErr_Format(PyExc_TypeError,
				     "__dir__() must return a list, not %.200s",
				     Py_Type(result)->tp_name);
			Py_DECREF(result);
			result = NULL;
		}
	}

	return result;
}

/* Implementation of dir() -- if obj is NULL, returns the names in the current
   (local) scope.  Otherwise, performs introspection of the object: returns a
   sorted list of attribute names (supposedly) accessible from the object
*/
PyObject *
PyObject_Dir(PyObject *obj)
{
	PyObject * result;

	if (obj == NULL)
		/* no object -- introspect the locals */
		result = _dir_locals();
	else
		/* object -- introspect the object */
		result = _dir_object(obj);

	assert(result == NULL || PyList_Check(result));

	if (result != NULL && PyList_Sort(result) != 0) {
		/* sorting the list failed */
		Py_DECREF(result);
		result = NULL;
	}
	
	return result;
}

/*
NoObject is usable as a non-NULL undefined value, used by the macro None.
There is (and should be!) no way to create other objects of this type,
so there is exactly one (which is indestructible, by the way).
(XXX This type and the type of NotImplemented below should be unified.)
*/

/* ARGSUSED */
static PyObject *
none_repr(PyObject *op)
{
	return PyUnicode_FromString("None");
}

/* ARGUSED */
static void
none_dealloc(PyObject* ignore)
{
	/* This should never get called, but we also don't want to SEGV if
	 * we accidently decref None out of existance.
	 */
	Py_FatalError("deallocating None");
}

static int
none_isshareable(PyObject *ignore)
{
	return 1;
}


static PyTypeObject PyNone_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"NoneType",
	0,
	0,
	none_dealloc,	/*tp_dealloc*/ /*never called*/
	0,		/*tp_print*/
	0,		/*tp_getattr*/
	0,		/*tp_setattr*/
	0,		/*tp_compare*/
	none_repr,	/*tp_repr*/
	0,		/*tp_as_number*/
	0,		/*tp_as_sequence*/
	0,		/*tp_as_mapping*/
	0,		/*tp_hash*/
	0,		/*tp_call*/
	0,		/*tp_str*/
	0,		/*tp_getattro*/
	0,		/*tp_setattro*/
	0,		/*tp_as_buffer*/
	Py_TPFLAGS_SHAREABLE,	/*tp_flags*/
	0,		/* tp_doc */
	0,		/* tp_traverse */
	0,		/* tp_clear */
	0,		/* tp_richcompare */
	0,		/* tp_weaklistoffset */
	0,		/* tp_iter */
	0,		/* tp_iternext */
	0,		/* tp_methods */
	0,		/* tp_members */
	0,		/* tp_getset */
	0,		/* tp_base */
	0,		/* tp_dict */
	0,		/* tp_descr_get */
	0,		/* tp_descr_set */
	0,		/* tp_dictoffset */
	0,		/* tp_init */
	0,		/* tp_new */
	0,		/* tp_is_gc */
	0,		/* tp_bases */
	0,		/* tp_mro */
	0,		/* tp_cache */
	0,		/* tp_subclasses */
	0,		/* tp_weaklist */
	none_isshareable,	/* tp_isshareable */
};

/*
PyObject _Py_NoneStruct = {
  _PyObject_EXTRA_INIT
  1, &PyNone_Type
};
*/
PyObject _Py_NoneStruct = PyObject_HEAD_INIT_NOCOMMA(&PyNone_Type);

/* NotImplemented is an object that can be used to signal that an
   operation is not implemented for the given type combination. */

static PyObject *
NotImplemented_repr(PyObject *op)
{
	return PyUnicode_FromString("NotImplemented");
}

static PyTypeObject PyNotImplemented_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"NotImplementedType",
	0,
	0,
	none_dealloc,	/*tp_dealloc*/ /*never called*/
	0,		/*tp_print*/
	0,		/*tp_getattr*/
	0,		/*tp_setattr*/
	0,		/*tp_compare*/
	NotImplemented_repr, /*tp_repr*/
	0,		/*tp_as_number*/
	0,		/*tp_as_sequence*/
	0,		/*tp_as_mapping*/
	0,		/*tp_hash */
	0,		/*tp_call*/
	0,		/*tp_str*/
	0,		/*tp_getattro*/
	0,		/*tp_setattro*/
	0,		/*tp_as_buffer*/
	Py_TPFLAGS_SHAREABLE,	/*tp_flags*/
	0,		/* tp_doc */
	0,		/* tp_traverse */
	0,		/* tp_clear */
	0,		/* tp_richcompare */
	0,		/* tp_weaklistoffset */
	0,		/* tp_iter */
	0,		/* tp_iternext */
	0,		/* tp_methods */
	0,		/* tp_members */
	0,		/* tp_getset */
	0,		/* tp_base */
	0,		/* tp_dict */
	0,		/* tp_descr_get */
	0,		/* tp_descr_set */
	0,		/* tp_dictoffset */
	0,		/* tp_init */
	0,		/* tp_new */
	0,		/* tp_is_gc */
	0,		/* tp_bases */
	0,		/* tp_mro */
	0,		/* tp_cache */
	0,		/* tp_subclasses */
	0,		/* tp_weaklist */
	none_isshareable,	/* tp_isshareable */
};

/*
PyObject _Py_NotImplementedStruct = {
	_PyObject_EXTRA_INIT
	1, &PyNotImplemented_Type
};
*/
PyObject _Py_NotImplementedStruct = PyObject_HEAD_INIT_NOCOMMA(&PyNotImplemented_Type);

extern PyTypeObject PyRange_Type;
extern PyTypeObject PyInterrupt_Type;

void
_Py_ReadyTypes(void)
{
	if (PyType_Ready(&PyType_Type) < 0)
		Py_FatalError("Can't initialize 'type'");

	if (PyType_Ready(&_PyWeakref_Type) < 0)
		Py_FatalError("Can't initialize 'weakref'");

	if (PyType_Ready(&_PyDeathQueueHandle_Type) < 0)
		Py_FatalError("Can't initialize 'deathqueuehandle'");

	if (PyType_Ready(&_PyDeathQueue_Type) < 0)
		Py_FatalError("Can't initialize 'deathqueue'");

	if (PyType_Ready(&PyBool_Type) < 0)
		Py_FatalError("Can't initialize 'bool'");

	if (PyType_Ready(&PyBytes_Type) < 0)
		Py_FatalError("Can't initialize 'bytes'");

	if (PyType_Ready(&PyString_Type) < 0)
		Py_FatalError("Can't initialize 'str'");

	if (PyType_Ready(&PyList_Type) < 0)
		Py_FatalError("Can't initialize 'list'");

	if (PyType_Ready(&PyNone_Type) < 0)
		Py_FatalError("Can't initialize type(None)");

	if (PyType_Ready(Py_Ellipsis->ob_type) < 0)
		Py_FatalError("Can't initialize type(Ellipsis)");

	if (PyType_Ready(&PyNotImplemented_Type) < 0)
		Py_FatalError("Can't initialize type(NotImplemented)");

	if (PyType_Ready(&PyCode_Type) < 0)
		Py_FatalError("Can't initialize 'code'");

	if (PyType_Ready(&PyRange_Type) < 0)
		Py_FatalError("Can't initialize 'range'");

	PySharedDict_Type.tp_base = &PyDict_Type;
	if (PyType_Ready(&PySharedDict_Type) < 0)
		Py_FatalError("Can't initialize 'shareddict'");

	PyMonitorMeta_Type.tp_base = &PyType_Type;
	if (PyType_Ready(&PyMonitorMeta_Type) < 0)
		Py_FatalError("Can't initialize 'MonitorMeta'");

	if (PyType_Ready(&PyMonitor_Type) < 0)
		Py_FatalError("Can't initialize 'Monitor'");

	if (PyType_Ready(&PyMonitorSpace_Type) < 0)
		Py_FatalError("Can't initialize 'MonitorSpace'");

	if (PyType_Ready(&PyBranch_Type) < 0)
		Py_FatalError("Can't initialize 'branch'");

	if (PyType_Ready(&PyInterrupt_Type) < 0)
		Py_FatalError("Can't initialize 'Interrupt' type");
}


/* Hack to force loading of cobject.o */
PyTypeObject *_Py_cobject_hack = &PyCObject_Type;


/* Hack to force loading of abstract.o */
Py_ssize_t (*_Py_abstract_hack)(PyObject *) = PyObject_Size;


/* Python's malloc wrappers (see pymem.h) */

#if 1
#else
void *
PyMem_Malloc(size_t nbytes)
{
	return PyMem_MALLOC(nbytes);
}

void *
PyMem_Realloc(void *p, size_t nbytes)
{
	return PyMem_REALLOC(p, nbytes);
}

void
PyMem_Free(void *p)
{
	PyMem_FREE(p);
}
#endif


/* These methods are used to control infinite recursion in repr, str, print,
   etc.  Container objects that may recursively contain themselves,
   e.g. builtin dictionaries and lists, should used Py_ReprEnter() and
   Py_ReprLeave() to avoid infinite recursion.

   Py_ReprEnter() returns 0 the first time it is called for a particular
   object and 1 every time thereafter.  It returns -1 if an exception
   occurred.  Py_ReprLeave() has no return value.

   See dictobject.c and listobject.c for examples of use.
*/

#define KEY "Py_Repr"

int
Py_ReprEnter(PyObject *obj)
{
	PyObject *dict;
	PyObject *list;
	Py_ssize_t i;

	dict = PyThreadState_GetDict();
	if (dict == NULL)
		return 0;
	list = PyDict_GetItemString(dict, KEY);
	if (list == NULL) {
		list = PyList_New(0);
		if (list == NULL)
			return -1;
		if (PyDict_SetItemString(dict, KEY, list) < 0)
			return -1;
		Py_DECREF(list);
	}
	i = PyList_GET_SIZE(list);
	while (--i >= 0) {
		if (PyList_GET_ITEM(list, i) == obj)
			return 1;
	}
	PyList_Append(list, obj);
	return 0;
}

void
Py_ReprLeave(PyObject *obj)
{
	PyObject *dict;
	PyObject *list;
	Py_ssize_t i;

	dict = PyThreadState_GetDict();
	if (dict == NULL)
		return;
	list = PyDict_GetItemString(dict, KEY);
	if (list == NULL || !PyList_Check(list))
		return;
	i = PyList_GET_SIZE(list);
	/* Count backwards because we always expect obj to be list[-1] */
	while (--i >= 0) {
		if (PyList_GET_ITEM(list, i) == obj) {
			PyList_SetSlice(list, i, i + 1, NULL);
			break;
		}
	}
}

#ifdef __cplusplus
}
#endif
