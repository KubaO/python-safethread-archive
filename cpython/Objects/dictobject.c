
/* Dictionary object implementation using a hash table */

/* The distribution includes a separate file, Objects/dictnotes.txt,
   describing explorations into dictionary design and optimization.
   It covers typical dictionary use patterns, the parameters for
   tuning dictionaries, and several ideas for possible optimizations.
*/

#include "Python.h"
#include "pystate.h"
#include "pythread.h"

typedef PyDictEntry dictentry;
typedef PyDictObject dictobject;

/* Set a key error with the specified argument, wrapping it in a
 * tuple automatically so that tuple keys are not unpacked as the
 * exception arguments. */
static void
set_key_error(PyObject *arg)
{
	PyObject *tup;
	tup = PyTuple_Pack(1, arg);
	if (!tup)
		return; /* caller will expect error to be set anyway */
	PyErr_SetObject(PyExc_KeyError, tup);
	Py_DECREF(tup);
}

static inline int
block_unshareable_keyvalue(PyObject *mp, PyObject *key, PyObject *value)
{
	if (PySharedDict_Check(mp) && !PyObject_IsShareable(key)) {
		PyErr_Format(PyExc_TypeError,
			"%.200s key must be shareable, "
			"'%s' object is not",
			mp->ob_type->tp_name, key->ob_type->tp_name);
		return 1;
	}
	if (PySharedDict_Check(mp) && !PyObject_IsShareable(value)) {
		PyErr_Format(PyExc_TypeError,
			"%.200s value must be shareable, "
			"'%s' object is not",
			mp->ob_type->tp_name, value->ob_type->tp_name);
		return 1;
	}
	return 0;
}

void
_pydictlock_initstate_read(PyDict_LockState *lockstate)
{
    lockstate->doing_write = 0;
    lockstate->skipped_lock = 0;
}

void
_pydictlock_initstate_write(PyDict_LockState *lockstate)
{
    lockstate->doing_write = 1;
    lockstate->skipped_lock = 0;
}

void
_pydictlock_initstate_notshared(PyDict_LockState *lockstate)
{
    lockstate->doing_write = -1;
    lockstate->skipped_lock = -1;
}

void
_pydictlock_acquire(PyDictObject *mp, PyDict_LockState *lockstate)
{
    /* XXX FIXME this should allow a NULL lockstate if mp isn't a shareddict */
    assert(lockstate);

    if (PySharedDict_Check(mp)) {
        PySharedDictObject *sd = (PySharedDictObject *)mp;
        assert(lockstate->doing_write == 0 || lockstate->doing_write == 1);
        assert(lockstate->skipped_lock == 0 || lockstate->skipped_lock == 1);

        if (lockstate->doing_write) {
            assert(!lockstate->skipped_lock);
            PyCritical_Enter(sd->crit);

            /* If the shareddict has entered readonly mode we use this
             * expensive fallback to reset it.  This *should* be fairly
             * rare. */
            while (AO_load_acquire(&sd->readonly_mode)) {
                PyCritical_Exit(sd->crit);
                PyState_StopTheWorld();
                AO_store_full(&sd->readonly_mode, 0);
                sd->read_count = 0;
                PyState_StartTheWorld();
                PyCritical_Enter(sd->crit);
            }

            sd->read_count = 0;
        } else {
            /* XXX FIXME this should use a stack-allocated critical
             * section if not using a real one */
            if (AO_load_acquire(&sd->readonly_mode))
                lockstate->skipped_lock = 1;
            else {
                PyCritical_Enter(sd->crit);
                if (AO_load_acquire(&sd->readonly_mode)) {
                    lockstate->skipped_lock = 1;
                    PyCritical_Exit(sd->crit);
                } else {
                    sd->read_count++;
                    //printf("Read count %d\n", sd->read_count);
                    sd->read_count = 1;  /* XXX FIXME currently disabled */
                    if (sd->read_count >= 10000) {
                        /* Enter read-only mode */
                        printf("Entering read-only mode%d\n", sd->read_count);
                        AO_store_full(&sd->readonly_mode, 1);
                        PyCritical_Exit(sd->crit);
                        lockstate->skipped_lock = 1;
                    } else
                        lockstate->skipped_lock = 0;
                }
            }
        }
    }

    /* It is an invariant that acquire/release are not called while the
     * dict would need to be resized. */
    assert(!(mp->ma_fill*3 >= (mp->ma_mask+1)*2));
}

void
_pydictlock_release(PyDictObject *mp, PyDict_LockState *lockstate)
{
    assert(lockstate);

    /* It is an invariant that acquire/release are not called while the
     * dict would need to be resized. */
    assert(!(mp->ma_fill*3 >= (mp->ma_mask+1)*2));

    if (PySharedDict_Check(mp)) {
        PySharedDictObject *sd = (PySharedDictObject *)mp;
        assert(lockstate->doing_write == 0 || lockstate->doing_write == 1);
        assert(lockstate->skipped_lock == 0 || lockstate->skipped_lock == 1);

        if (lockstate->doing_write) {
            assert(!lockstate->skipped_lock);
            PyCritical_Exit(sd->crit);
        } else {
            /* XXX FIXME exit stack-allocated critical */
            if (!lockstate->skipped_lock)
                PyCritical_Exit(sd->crit);
        }
    }
}

/* Define this out if you don't want conversion statistics on exit. */
#undef SHOW_CONVERSION_COUNTS

/* See large comment block below.  This must be >= 1. */
#define PERTURB_SHIFT 5

/*
Major subtleties ahead:  Most hash schemes depend on having a "good" hash
function, in the sense of simulating randomness.  Python doesn't:  its most
important hash functions (for strings and ints) are very regular in common
cases:

  >>> map(hash, (0, 1, 2, 3))
  [0, 1, 2, 3]
  >>> map(hash, ("namea", "nameb", "namec", "named"))
  [-1658398457, -1658398460, -1658398459, -1658398462]
  >>>

This isn't necessarily bad!  To the contrary, in a table of size 2**i, taking
the low-order i bits as the initial table index is extremely fast, and there
are no collisions at all for dicts indexed by a contiguous range of ints.
The same is approximately true when keys are "consecutive" strings.  So this
gives better-than-random behavior in common cases, and that's very desirable.

OTOH, when collisions occur, the tendency to fill contiguous slices of the
hash table makes a good collision resolution strategy crucial.  Taking only
the last i bits of the hash code is also vulnerable:  for example, consider
the list [i << 16 for i in range(20000)] as a set of keys.  Since ints are 
their own hash codes, and this fits in a dict of size 2**15, the last 15 bits
 of every hash code are all 0:  they *all* map to the same table index.

But catering to unusual cases should not slow the usual ones, so we just take
the last i bits anyway.  It's up to collision resolution to do the rest.  If
we *usually* find the key we're looking for on the first try (and, it turns
out, we usually do -- the table load factor is kept under 2/3, so the odds
are solidly in our favor), then it makes best sense to keep the initial index
computation dirt cheap.

The first half of collision resolution is to visit table indices via this
recurrence:

    j = ((5*j) + 1) mod 2**i

For any initial j in range(2**i), repeating that 2**i times generates each
int in range(2**i) exactly once (see any text on random-number generation for
proof).  By itself, this doesn't help much:  like linear probing (setting
j += 1, or j -= 1, on each loop trip), it scans the table entries in a fixed
order.  This would be bad, except that's not the only thing we do, and it's
actually *good* in the common cases where hash keys are consecutive.  In an
example that's really too small to make this entirely clear, for a table of
size 2**3 the order of indices is:

    0 -> 1 -> 6 -> 7 -> 4 -> 5 -> 2 -> 3 -> 0 [and here it's repeating]

If two things come in at index 5, the first place we look after is index 2,
not 6, so if another comes in at index 6 the collision at 5 didn't hurt it.
Linear probing is deadly in this case because there the fixed probe order
is the *same* as the order consecutive keys are likely to arrive.  But it's
extremely unlikely hash codes will follow a 5*j+1 recurrence by accident,
and certain that consecutive hash codes do not.

The other half of the strategy is to get the other bits of the hash code
into play.  This is done by initializing a (unsigned) vrbl "perturb" to the
full hash code, and changing the recurrence to:

    j = (5*j) + 1 + perturb;
    perturb >>= PERTURB_SHIFT;
    use j % 2**i as the next table index;

Now the probe sequence depends (eventually) on every bit in the hash code,
and the pseudo-scrambling property of recurring on 5*j+1 is more valuable,
because it quickly magnifies small differences in the bits that didn't affect
the initial index.  Note that because perturb is unsigned, if the recurrence
is executed often enough perturb eventually becomes and remains 0.  At that
point (very rarely reached) the recurrence is on (just) 5*j+1 again, and
that's certain to find an empty slot eventually (since it generates every int
in range(2**i), and we make sure there's always at least one empty slot).

Selecting a good value for PERTURB_SHIFT is a balancing act.  You want it
small so that the high bits of the hash code continue to affect the probe
sequence across iterations; but you want it large so that in really bad cases
the high-order hash bits have an effect on early iterations.  5 was "the
best" in minimizing total collisions across experiments Tim Peters ran (on
both normal and pathological cases), but 4 and 6 weren't significantly worse.

Historical: Reimer Behrends contributed the idea of using a polynomial-based
approach, using repeated multiplication by x in GF(2**n) where an irreducible
polynomial for each table size was chosen such that x was a primitive root.
Christian Tismer later extended that to use division by x instead, as an
efficient way to get the high bits of the hash code into play.  This scheme
also gave excellent collision statistics, but was more expensive: two if-tests
were required inside the loop; computing "the next" index took about the same
number of operations but without as much potential parallelism (e.g.,
computing 5*j can go on at the same time as computing 1+perturb in the above,
and then shifting perturb can be done while the table index is being masked);
and the dictobject struct required a member to hold the table's polynomial.
In Tim's experiments the current scheme ran faster, produced equally good
collision statistics, needed less code & used less memory.

Theoretical Python 2.5 headache:  hash codes are only C "long", but
sizeof(Py_ssize_t) > sizeof(long) may be possible.  In that case, and if a
dict is genuinely huge, then only the slots directly reachable via indexing
by a C long can be the first slot in a probe sequence.  The probe sequence
will still eventually reach every slot in the table, but the collision rate
on initial probes may be much higher than this scheme was designed for.
Getting a hash code as fat as Py_ssize_t is the only real cure.  But in
practice, this probably won't make a lick of difference for many years (at
which point everyone will have terabytes of RAM on 64-bit boxes).
*/

/* Object used as dummy key to fill deleted entries */
static PyObject *dummy = NULL; /* Initialized by first call to newdictobject() */

#ifdef Py_REF_DEBUG
PyObject *
_PyDict_Dummy(void)
{
	return dummy;
}
#endif

/* forward declarations */
static dictentry *
lookdict_unicode(dictobject *mp, PyObject *key, long hash,
    PyDict_LockState *lockstate);

#ifdef SHOW_CONVERSION_COUNTS
static long created = 0L;
static long converted = 0L;

static void
show_counts(void)
{
	fprintf(stderr, "created %ld string dicts\n", created);
	fprintf(stderr, "converted %ld to normal dicts\n", converted);
	fprintf(stderr, "%.2f%% conversion rate\n", (100.0*converted)/created);
}
#endif

/* Initialization macros.
   There are two ways to create a dict:  PyDict_New() is the main C API
   function, and the tp_new slot maps to dict_new().  In the latter case we
   can save a little time over what PyDict_New does because it's guaranteed
   that the PyDictObject struct is already zeroed out.
   Everyone except dict_new() should use EMPTY_TO_MINSIZE (unless they have
   an excellent reason not to).
*/

#define INIT_NONZERO_DICT_SLOTS(mp) do {				\
	(mp)->ma_table = (mp)->ma_smalltable;				\
	(mp)->ma_mask = PyDict_MINSIZE - 1;				\
    } while(0)

#define EMPTY_TO_MINSIZE(mp) do {					\
	memset((mp)->ma_smalltable, 0, sizeof((mp)->ma_smalltable));	\
	(mp)->ma_used = (mp)->ma_fill = 0;				\
	(mp)->ma_rebuilds = 0;						\
	INIT_NONZERO_DICT_SLOTS(mp);					\
    } while(0)

//#define USE_DICT_FREELIST

#ifdef USE_DICT_FREELIST
/* Dictionary reuse scheme to save calls to malloc, free, and memset */
#define MAXFREEDICTS 80
static PyDictObject *free_dicts[MAXFREEDICTS];
static int num_free_dicts = 0;
/* This lock is only used while the GIL is already held */
static PyThread_type_lock free_dicts_lock;
#endif

PyObject *
PyDict_New(void)
{
	register dictobject *mp;
	if (dummy == NULL) { /* Auto-initialize dummy */
		dummy = PyUnicode_FromString("<dummy key>");
		if (dummy == NULL)
			return NULL;
#ifdef SHOW_CONVERSION_COUNTS
		Py_AtExit(show_counts);
#endif
	}
#ifdef USE_DICT_FREELIST
	PyThread_lock_acquire(free_dicts_lock);
	if (num_free_dicts) {
		mp = free_dicts[--num_free_dicts];
		PyThread_lock_release(free_dicts_lock);
		assert (mp != NULL);
		assert (Py_Type(mp) == &PyDict_Type);
		_Py_NewReference((PyObject *)mp);
		if (mp->ma_fill) {
			EMPTY_TO_MINSIZE(mp);
		}
		assert (mp->ma_used == 0);
		assert (mp->ma_table == mp->ma_smalltable);
		assert (mp->ma_mask == PyDict_MINSIZE - 1);
	} else {
		PyThread_lock_release(free_dicts_lock);
#endif
		mp = PyObject_NEW(dictobject, &PyDict_Type);
		if (mp == NULL)
			return NULL;
		EMPTY_TO_MINSIZE(mp);
#ifdef USE_DICT_FREELIST
	}
#endif
	mp->ma_lookup = lookdict_unicode;
#ifdef SHOW_CONVERSION_COUNTS
	++created;
#endif
	return (PyObject *)mp;
}

/*
The basic lookup function used by all operations.
This is based on Algorithm D from Knuth Vol. 3, Sec. 6.4.
Open addressing is preferred over chaining since the link overhead for
chaining would be substantial (100% with typical malloc overhead).

The initial probe index is computed as hash mod the table size. Subsequent
probe indices are computed as explained earlier.

All arithmetic on hash should ignore overflow.

The details in this version are due to Tim Peters, building on many past
contributions by Reimer Behrends, Jyrki Alakuijala, Vladimir Marangozov and
Christian Tismer.

lookdict() is general-purpose, and may return NULL if (and only if) a
comparison raises an exception (this was new in Python 2.5).
lookdict_unicode() below is specialized to string keys, comparison of which can
never raise an exception; that function can never return NULL.  For both, when
the key isn't found a dictentry* is returned for which the me_value field is
NULL; this is the slot in the dict at which the key would have been found, and
the caller can (if it wishes) add the <key, value> pair to the returned
dictentry*.
*/
static dictentry *
lookdict(dictobject *mp, PyObject *key, register long hash,
		PyDict_LockState *lockstate)
{
	register size_t i;
	register size_t perturb;
	register dictentry *freeslot;
	register size_t mask;
	dictentry *ep0;
	register dictentry *ep;
	register int cmp;
	PyObject *startkey;
	unsigned long long rebuilds;

start:
	mask = (size_t)mp->ma_mask;
	ep0 = mp->ma_table;
	rebuilds = mp->ma_rebuilds;
	i = (size_t)hash & mask;
	ep = &ep0[i];
	if (ep->me_key == NULL || ep->me_key == key)
		return ep;

	if (ep->me_key == dummy)
		freeslot = ep;
	else {
		if (ep->me_hash == hash) {
			startkey = ep->me_key;
			Py_INCREF(startkey);
			_pydictlock_release(mp, lockstate);
			cmp = PyObject_RichCompareBool(startkey, key, Py_EQ);
			_pydictlock_acquire(mp, lockstate);
			if (cmp < 0) {
				_pydictlock_release(mp, lockstate);
				Py_DECREF(startkey);
				_pydictlock_acquire(mp, lockstate);
				return NULL;
			}
			/* XXX use branch hinting? */
			if (rebuilds != mp->ma_rebuilds || ep->me_key != startkey) {
				/* The compare did major nasty stuff to the
				 * dict:  start over.
				 * XXX A clever adversary could prevent this
				 * XXX from terminating.
				 */
				_pydictlock_release(mp, lockstate);
				Py_DECREF(startkey);
				_pydictlock_acquire(mp, lockstate);
				goto start;
			}
			assert(ep0 == mp->ma_table);
			assert(mask == (size_t)mp->ma_mask);
			Py_DECREF(startkey);
			if (cmp > 0)
				return ep;
		}
		freeslot = NULL;
	}

	/* In the loop, me_key == dummy is by far (factor of 100s) the
	   least likely outcome, so test for that last. */
	for (perturb = hash; ; perturb >>= PERTURB_SHIFT) {
		i = (i << 2) + i + perturb + 1;
		ep = &ep0[i & mask];
		if (ep->me_key == NULL)
			return freeslot == NULL ? ep : freeslot;
		if (ep->me_key == key)
			return ep;
		if (ep->me_hash == hash && ep->me_key != dummy) {
			startkey = ep->me_key;
			Py_INCREF(startkey);
			_pydictlock_release(mp, lockstate);
			cmp = PyObject_RichCompareBool(startkey, key, Py_EQ);
			_pydictlock_acquire(mp, lockstate);
			if (cmp < 0) {
				_pydictlock_release(mp, lockstate);
				Py_DECREF(startkey);
				_pydictlock_acquire(mp, lockstate);
				return NULL;
			}
			/* XXX use branch hinting? */
			if (rebuilds != mp->ma_rebuilds || ep->me_key != startkey) {
				/* The compare did major nasty stuff to the
				 * dict:  start over.
				 * XXX A clever adversary could prevent this
				 * XXX from terminating.
 				 */
				_pydictlock_release(mp, lockstate);
				Py_DECREF(startkey);
				_pydictlock_acquire(mp, lockstate);
				goto start;
			}
			Py_DECREF(startkey);
			assert(ep0 == mp->ma_table);
			assert(mask == (size_t)mp->ma_mask);
			if (cmp > 0)
				return ep;
		}
		else if (ep->me_key == dummy && freeslot == NULL)
			freeslot = ep;
	}
	assert(0);	/* NOT REACHED */
	return 0;
}

/* Return 1 if two unicode objects are equal, 0 if not. */
static int
unicode_eq(PyObject *aa, PyObject *bb)
{
	PyUnicodeObject *a = (PyUnicodeObject *)aa;
	PyUnicodeObject *b = (PyUnicodeObject *)bb;

	if (a->length != b->length)
		return 0;
	if (a->length == 0)
		return 1;
	if (a->str[0] != b->str[0])
		return 0;
	if (a->length == 1)
		return 1;
	return memcmp(a->str, b->str, a->length * sizeof(Py_UNICODE)) == 0;
}


/*
 * Hacked up version of lookdict which can assume keys are always
 * unicodes; this assumption allows testing for errors during
 * PyObject_RichCompareBool() to be dropped; unicode-unicode
 * comparisons never raise exceptions.  This also means we don't need
 * to go through PyObject_RichCompareBool(); we can always use
 * unicode_eq() directly.
 *
 * This is valuable because dicts with only unicode keys are very common.
 */
static dictentry *
lookdict_unicode(dictobject *mp, PyObject *key, register long hash,
		PyDict_LockState *lockstate)
{
	register size_t i;
	register size_t perturb;
	register dictentry *freeslot;
	register size_t mask = (size_t)mp->ma_mask;
	dictentry *ep0 = mp->ma_table;
	register dictentry *ep;

	assert(lockstate);

	/* Make sure this function doesn't have to handle non-unicode keys,
	   including subclasses of str; e.g., one reason to subclass
	   unicodes is to override __eq__, and for speed we don't cater to
	   that here. */
	if (!PyUnicode_CheckExact(key)) {
#ifdef SHOW_CONVERSION_COUNTS
		++converted;
#endif
		mp->ma_lookup = lookdict;
		return lookdict(mp, key, hash, lockstate);
	}
	i = hash & mask;
	ep = &ep0[i];
	if (ep->me_key == NULL || ep->me_key == key)
		return ep;
	if (ep->me_key == dummy)
		freeslot = ep;
	else {
		if (ep->me_hash == hash && unicode_eq(ep->me_key, key))
			return ep;
		freeslot = NULL;
	}

	/* In the loop, me_key == dummy is by far (factor of 100s) the
	   least likely outcome, so test for that last. */
	for (perturb = hash; ; perturb >>= PERTURB_SHIFT) {
		i = (i << 2) + i + perturb + 1;
		ep = &ep0[i & mask];
		if (ep->me_key == NULL)
			return freeslot == NULL ? ep : freeslot;
		if (ep->me_key == key
		    || (ep->me_hash == hash
		        && ep->me_key != dummy
			&& unicode_eq(ep->me_key, key)))
			return ep;
		if (ep->me_key == dummy && freeslot == NULL)
			freeslot = ep;
	}
	assert(0);	/* NOT REACHED */
	return 0;
}

/*
Internal routine to insert a new item into the table.
Used both by the internal resize routine and by the public insert routine.
Eats a reference to key and one to value.
Returns -1 if an error occurred, or 0 on success.
*/
static int
insertdict(register dictobject *mp, PyObject *key, long hash,
		PyObject *value, PyDict_LockState *lockstate)
{
	PyObject *old_value;
	register dictentry *ep;
	typedef PyDictEntry *(*lookupfunc)(PyDictObject *, PyObject *, long);

	assert(mp->ma_lookup != NULL);
	ep = mp->ma_lookup(mp, key, hash, lockstate);
	if (ep == NULL) {
		_pydictlock_release(mp, lockstate);
		Py_DECREF(key);
		Py_DECREF(value);
		_pydictlock_acquire(mp, lockstate);
		return -1;
	}

	if (ep->me_value != NULL) {
		old_value = ep->me_value;
		ep->me_value = value;
		_pydictlock_release(mp, lockstate);
		Py_DECREF(old_value); /* which **CAN** re-enter */
		Py_DECREF(key);
		_pydictlock_acquire(mp, lockstate);
	} else {
		if (ep->me_key == NULL)
			mp->ma_fill++;
		else {
			assert(ep->me_key == dummy);
			Py_DECREF(dummy);
		}
		ep->me_key = key;
		ep->me_hash = (Py_ssize_t)hash;
		ep->me_value = value;
		mp->ma_used++;
	}
	return 0;
}

/*
Internal routine used by dictresize() to insert an item which is
known to be absent from the dict.  This routine also assumes that
the dict contains no deleted entries.  Besides the performance benefit,
using insertdict() in dictresize() is dangerous (SF bug #1456209).
Note that no refcounts are changed by this routine; if needed, the caller
is responsible for incref'ing `key` and `value`.
*/
static void
insertdict_clean(register dictobject *mp, PyObject *key, long hash,
		 PyObject *value)
{
	register size_t i;
	register size_t perturb;
	register size_t mask = (size_t)mp->ma_mask;
	dictentry *ep0 = mp->ma_table;
	register dictentry *ep;

	i = hash & mask;
	ep = &ep0[i];
	for (perturb = hash; ep->me_key != NULL; perturb >>= PERTURB_SHIFT) {
		i = (i << 2) + i + perturb + 1;
		ep = &ep0[i & mask];
	}
	assert(ep->me_value == NULL);
	mp->ma_fill++;
	ep->me_key = key;
	ep->me_hash = (Py_ssize_t)hash;
	ep->me_value = value;
	mp->ma_used++;
}

/*
Restructure the table by allocating a new table and reinserting all
items again.  When entries have been deleted, the new table may
actually be smaller than the old one.
*/
static int
dictresize(dictobject *mp, Py_ssize_t minused)
{
	Py_ssize_t newsize;
	dictentry *oldtable, *newtable, *ep;
	Py_ssize_t i;
	int is_oldtable_malloced;
	dictentry small_copy[PyDict_MINSIZE];

	assert(minused >= 0);

	/* Find the smallest table size > minused. */
	for (newsize = PyDict_MINSIZE;
	     newsize <= minused && newsize > 0;
	     newsize <<= 1)
		;
	if (newsize <= 0) {
		PyErr_NoMemory();
		return -1;
	}

	/* Get space for a new table. */
	oldtable = mp->ma_table;
	assert(oldtable != NULL);
	is_oldtable_malloced = oldtable != mp->ma_smalltable;

	if (newsize == PyDict_MINSIZE) {
		/* A large table is shrinking, or we can't get any smaller. */
		newtable = mp->ma_smalltable;
		if (newtable == oldtable) {
			if (mp->ma_fill == mp->ma_used) {
				/* No dummies, so no point doing anything. */
				return 0;
			}
			/* We're not going to resize it, but rebuild the
			   table anyway to purge old dummy entries.
			   Subtle:  This is *necessary* if fill==size,
			   as lookdict needs at least one virgin slot to
			   terminate failing searches.  If fill < size, it's
			   merely desirable, as dummies slow searches. */
			assert(mp->ma_fill > mp->ma_used);
			memcpy(small_copy, oldtable, sizeof(small_copy));
			oldtable = small_copy;
		}
	}
	else {
		newtable = PyMem_NEW(dictentry, newsize);
		if (newtable == NULL) {
			PyErr_NoMemory();
			return -1;
		}
	}

	/* Make the dict empty, using the new table. */
	assert(newtable != oldtable);
	mp->ma_table = newtable;
	mp->ma_mask = newsize - 1;
	memset(newtable, 0, sizeof(dictentry) * newsize);
	mp->ma_used = 0;
	i = mp->ma_fill;
	mp->ma_fill = 0;
	mp->ma_rebuilds++;

	/* Copy the data over; this is refcount-neutral for active entries;
	   dummy entries aren't copied over, of course */
	for (ep = oldtable; i > 0; ep++) {
		if (ep->me_value != NULL) {	/* active entry */
			--i;
			insertdict_clean(mp, ep->me_key, (long)ep->me_hash,
					 ep->me_value);
		}
		else if (ep->me_key != NULL) {	/* dummy entry */
			--i;
			assert(ep->me_key == dummy);
			Py_DECREF(ep->me_key);
		}
		/* else key == value == NULL:  nothing to do */
	}

	if (is_oldtable_malloced)
		PyMem_DEL(oldtable);
	return 0;
}

/* Note that, for historical reasons, PyDict_GetItem() suppresses all errors
 * that may occur (originally dicts supported only string keys, and exceptions
 * weren't possible).  So, while the original intent was that a NULL return
 * meant the key wasn't present, in reality it can mean that, or that an error
 * (suppressed) occurred while computing the key's hash, or that some error
 * (suppressed) occurred when comparing keys in the dict's internal probe
 * sequence.  A nasty example of the latter is when a Python-coded comparison
 * function hits a stack-depth error, which can cause this to return NULL
 * even if the key is present.
 */
PyObject *
PyDict_GetItem(PyObject *op, PyObject *key)
{
	PyThreadState *tstate = PyThreadState_Get();
	long hash;
	dictobject *mp = (dictobject *)op;
	dictentry *ep;
	PyDict_LockState lockstate;

	if (!PyDict_Check(op))
		return NULL;
	if (!PyUnicode_CheckExact(key) ||
	    (hash = ((PyUnicodeObject *) key)->hash) == -1)
	{
		hash = PyObject_Hash(key);
		if (hash == -1) {
			PyErr_Clear();
			return NULL;
		}
	}

	_pydictlock_initstate_notshared(&lockstate);

	/* We can arrive here with a NULL tstate during initialization:
	   try running "python -Wi" for an example related to string
	   interning.  Let's just hope that no exception occurs then... */
	/* XXX It's now impossible to have a NULL tstate */
	if (tstate != NULL && tstate->curexc_type != NULL) {
		/* preserve the existing exception */
		PyObject *err_type, *err_value, *err_tb;
		PyErr_Fetch(&err_type, &err_value, &err_tb);
		ep = (mp->ma_lookup)(mp, key, hash, &lockstate);
		/* ignore errors */
		PyErr_Restore(err_type, err_value, err_tb);
		if (ep == NULL)
			return NULL;
	}
	else {
		ep = (mp->ma_lookup)(mp, key, hash, &lockstate);
		if (ep == NULL) {
			PyErr_Clear();
			return NULL;
		}
	}
	return ep->me_value;
}

/* Variant of PyDict_GetItem() that doesn't suppress exceptions.
   This returns NULL *with* an exception set if an exception occurred.
   It returns NULL *without* an exception set if the key wasn't present.
*/
PyObject *
PyDict_GetItemWithError(PyObject *op, PyObject *key)
{
	long hash;
	dictobject *mp = (dictobject *)op;
	dictentry *ep;
	PyDict_LockState lockstate;

	if (!PyDict_Check(op)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	if (!PyUnicode_CheckExact(key) ||
	    (hash = ((PyUnicodeObject *) key)->hash) == -1)
	{
		hash = PyObject_Hash(key);
		if (hash == -1) {
			return NULL;
		}
	}

	_pydictlock_initstate_notshared(&lockstate);

	ep = (mp->ma_lookup)(mp, key, hash, &lockstate);
	if (ep == NULL)
		return NULL;
	return ep->me_value;
}

/* Yet another variant of PyDict_GetItem().  Return values:
 * -1 Error, exception set (value set to NULL)
 *  0 Success (value filled in with *NEW* reference)
 * +1 Not found, no exception set (value set to NULL)
 */
int
PyDict_GetItemEx(PyObject *op, PyObject *key, PyObject **value)
{
    long hash;
    dictobject *mp = (dictobject *)op;
    dictentry *ep;
    PyDict_LockState lockstate;

    *value = NULL;

    if (!PyDict_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (!PyUnicode_CheckExact(key) ||
            (hash = ((PyUnicodeObject *) key)->hash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1) {
            return -1;
        }
    }

    _pydictlock_initstate_read(&lockstate);

    _pydictlock_acquire(mp, &lockstate);
    ep = (mp->ma_lookup)(mp, key, hash, &lockstate);
    if (ep == NULL) {
        _pydictlock_release(mp, &lockstate);
        return -1;
    }

    *value = ep->me_value;
    Py_XINCREF(*value);
    _pydictlock_release(mp, &lockstate);

    if (*value)
        return 0;
    else
        return 1;
}

/* CAUTION: PyDict_SetItem() must guarantee that it won't resize the
 * dictionary if it's merely replacing the value for an existing key.
 * This means that it's safe to loop over a dictionary with PyDict_Next()
 * and occasionally replace a value -- but you can't insert new keys or
 * remove them.
 */
int
PyDict_SetItem(register PyObject *op, PyObject *key, PyObject *value)
{
	PyThreadState *tstate = PyThreadState_Get();
	register dictobject *mp;
	register long hash;
	register Py_ssize_t n_used;
	PyDict_LockState lockstate;
	int result;

	if (!PyDict_Check(op)) {
		PyErr_BadInternalCall();
		return -1;
	}
	assert(key);
	assert(value);
	mp = (dictobject *)op;
	if (block_unshareable_keyvalue(op, key, value))
		return -1;
	if (!PyUnicode_CheckExact(key) ||
			(hash = ((PyUnicodeObject *) key)->hash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return -1;
	}
	_pydictlock_initstate_write(&lockstate);

	_pydictlock_acquire(mp, &lockstate);
	assert(mp->ma_fill <= mp->ma_mask);  /* at least one empty slot */
	n_used = mp->ma_used;
	Py_INCREFTS(value);
	Py_INCREFTS(key);
	if (insertdict(mp, key, hash, value, &lockstate) != 0) {
		_pydictlock_release(mp, &lockstate);
		return -1;
	}

	/* If we added a key, we can safely resize.  Otherwise just return!
	 * If fill >= 2/3 size, adjust size.  Normally, this doubles or
	 * quaduples the size, but it's also possible for the dict to shrink
	 * (if ma_fill is much larger than ma_used, meaning a lot of dict
	 * keys have been * deleted).
	 *
	 * Quadrupling the size improves average dictionary sparseness
	 * (reducing collisions) at the cost of some memory and iteration
	 * speed (which loops over every possible entry).  It also halves
	 * the number of expensive resize operations in a growing dictionary.
	 *
	 * Very large dictionaries (over 50K items) use doubling instead.
	 * This may help applications with severe memory constraints.
	 */
	if (!(mp->ma_used > n_used && mp->ma_fill*3 >= (mp->ma_mask+1)*2)) {
		_pydictlock_release(mp, &lockstate);
		return 0;
	}
	result = dictresize(mp, (mp->ma_used > 50000 ? 2 : 4) * mp->ma_used);
	_pydictlock_release(mp, &lockstate);
	return result;
}

int
PyDict_DelItem(PyObject *op, PyObject *key)
{
	register dictobject *mp;
	register long hash;
	register dictentry *ep;
	PyObject *old_value, *old_key;
	PyDict_LockState lockstate;

	if (!PyDict_Check(op)) {
		PyErr_BadInternalCall();
		return -1;
	}
	assert(key);
	if (!PyUnicode_CheckExact(key) ||
	    (hash = ((PyUnicodeObject *) key)->hash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return -1;
	}
	mp = (dictobject *)op;
	_pydictlock_initstate_write(&lockstate);

	_pydictlock_acquire(mp, &lockstate);
	ep = (mp->ma_lookup)(mp, key, hash, &lockstate);
	if (ep == NULL) {
		_pydictlock_release(mp, &lockstate);
		return -1;
	}
	if (ep->me_value == NULL) {
		_pydictlock_release(mp, &lockstate);
		set_key_error(key);
		return -1;
	}
	old_key = ep->me_key;
	Py_INCREF(dummy);
	ep->me_key = dummy;
	old_value = ep->me_value;
	ep->me_value = NULL;
	mp->ma_used--;
	_pydictlock_release(mp, &lockstate);
	Py_DECREF(old_value);
	Py_DECREF(old_key);
	return 0;
}

void
PyDict_Clear(PyObject *op)
{
	dictobject *mp;
	dictentry *ep, *table;
	int table_is_malloced;
	Py_ssize_t fill;
	dictentry small_copy[PyDict_MINSIZE];
#ifdef Py_DEBUG
	Py_ssize_t i, n;
#endif
	PyDict_LockState lockstate;

	if (!PyDict_Check(op))
		return;
	mp = (dictobject *)op;
	_pydictlock_initstate_write(&lockstate);

	_pydictlock_acquire(mp, &lockstate);
#ifdef Py_DEBUG
	n = mp->ma_mask + 1;
	i = 0;
#endif

	table = mp->ma_table;
	assert(table != NULL);
	table_is_malloced = table != mp->ma_smalltable;

	/* This is delicate.  During the process of clearing the dict,
	 * decrefs can cause the dict to mutate.  To avoid fatal confusion
	 * (voice of experience), we have to make the dict empty before
	 * clearing the slots, and never refer to anything via mp->xxx while
	 * clearing.
	 */
	fill = mp->ma_fill;
	if (table_is_malloced)
		EMPTY_TO_MINSIZE(mp);

	else if (fill > 0) {
		/* It's a small table with something that needs to be cleared.
		 * Afraid the only safe way is to copy the dict entries into
		 * another small table first.
		 */
		memcpy(small_copy, table, sizeof(small_copy));
		table = small_copy;
		EMPTY_TO_MINSIZE(mp);
	}
	/* else it's a small table that's already empty */
	_pydictlock_release(mp, &lockstate);

	/* Now we can finally clear things.  If C had refcounts, we could
	 * assert that the refcount on table is 1 now, i.e. that this function
	 * has unique access to it, so decref side-effects can't alter it.
	 */
	for (ep = table; fill > 0; ++ep) {
#ifdef Py_DEBUG
		assert(i < n);
		++i;
#endif
		if (ep->me_key) {
			--fill;
			Py_DECREF(ep->me_key);
			Py_XDECREF(ep->me_value);
		}
#ifdef Py_DEBUG
		else
			assert(ep->me_value == NULL);
#endif
	}

	if (table_is_malloced)
		PyMem_DEL(table);
}

/*
 * Iterate over a dict.  Use like so:
 *
 *     Py_ssize_t i;
 *     PyObject *key, *value;
 *     i = 0;   # important!  i should not otherwise be changed by you
 *     while (PyDict_Next(yourdict, &i, &key, &value)) {
 *              Refer to borrowed references in key and value.
 *     }
 *
 * CAUTION:  In general, it isn't safe to use PyDict_Next in a loop that
 * mutates the dict.  One exception:  it is safe if the loop merely changes
 * the values associated with the keys (but doesn't insert new keys or
 * delete keys), via PyDict_SetItem().
 */
int
PyDict_Next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue)
{
	register Py_ssize_t i;
	register Py_ssize_t mask;
	register dictentry *ep;
	PyDict_LockState lockstate;

	if (!PyDict_Check(op))
		return 0;
	i = *ppos;
	if (i < 0)
		return 0;

	_pydictlock_initstate_notshared(&lockstate);
	_pydictlock_acquire((dictobject *)op, &lockstate);
	_pydictlock_release((dictobject *)op, &lockstate);

	ep = ((dictobject *)op)->ma_table;
	mask = ((dictobject *)op)->ma_mask;
	while (i <= mask && ep[i].me_value == NULL)
		i++;
	*ppos = i+1;
	if (i > mask)
		return 0;
	if (pkey)
		*pkey = ep[i].me_key;
	if (pvalue)
		*pvalue = ep[i].me_value;
	return 1;
}

/* Internal version of PyDict_Next that returns a hash value in addition to the key and value.*/
int
_PyDict_Next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue, long *phash)
{
	register Py_ssize_t i;
	register Py_ssize_t mask;
	register dictentry *ep;
	PyDict_LockState lockstate;

	if (!PyDict_Check(op))
		return 0;
	i = *ppos;
	if (i < 0)
		return 0;

	_pydictlock_initstate_notshared(&lockstate);
	_pydictlock_acquire((dictobject *)op, &lockstate);
	_pydictlock_release((dictobject *)op, &lockstate);

	ep = ((dictobject *)op)->ma_table;
	mask = ((dictobject *)op)->ma_mask;
	while (i <= mask && ep[i].me_value == NULL)
		i++;
	*ppos = i+1;
	if (i > mask)
		return 0;
        *phash = (long)(ep[i].me_hash);
	if (pkey)
		*pkey = ep[i].me_key;
	if (pvalue)
		*pvalue = ep[i].me_value;
	return 1;
}

/* Variant of PyDict_Next that provides *NEW* references to key and value */
int
PyDict_NextEx(PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue)
{
	register Py_ssize_t i;
	register Py_ssize_t mask;
	register dictentry *ep;
	PyDict_LockState lockstate;

	if (!PyDict_Check(op))
		return 0;
	i = *ppos;
	if (i < 0)
		return 0;

	_pydictlock_initstate_read(&lockstate);

	_pydictlock_acquire((dictobject *)op, &lockstate);
	ep = ((dictobject *)op)->ma_table;
	mask = ((dictobject *)op)->ma_mask;
	while (i <= mask && ep[i].me_value == NULL)
		i++;
	*ppos = i+1;
	if (i > mask) {
		_pydictlock_release((dictobject *)op, &lockstate);
		return 0;
	}

	if (pkey) {
		*pkey = ep[i].me_key;
		Py_INCREF(*pkey);
	}
	if (pvalue) {
		*pvalue = ep[i].me_value;
		Py_INCREF(*pvalue);
	}

	_pydictlock_release((dictobject *)op, &lockstate);
	return 1;
}

/* Methods */

static void
dict_dealloc(register dictobject *mp)
{
	register dictentry *ep;
	Py_ssize_t fill = mp->ma_fill;
	for (ep = mp->ma_table; fill > 0; ep++) {
		if (ep->me_key) {
			--fill;
			Py_DECREF(ep->me_key);
			Py_XDECREF(ep->me_value);
		}
	}
	if (mp->ma_table != mp->ma_smalltable)
		PyMem_DEL(mp->ma_table);
#ifdef USE_DICT_FREELIST
	PyThread_lock_acquire(free_dicts_lock);
	if (num_free_dicts < MAXFREEDICTS && Py_Type(mp) == &PyDict_Type) {
		free_dicts[num_free_dicts++] = mp;
		PyThread_lock_release(free_dicts_lock);
	} else {
		PyThread_lock_release(free_dicts_lock);
#endif
		PyObject_DEL(mp);
#ifdef USE_DICT_FREELIST
	}
#endif
}

static void
shareddict_dealloc(PySharedDictObject *mp)
{
    PyCritical_Free(mp->crit);
    dict_dealloc((PyDictObject *)mp);
}

static PyObject *
dict_repr(dictobject *mp)
{
	Py_ssize_t i;
	PyObject *s, *temp, *colon = NULL;
	PyObject *pieces = NULL, *result = NULL;
	PyObject *key, *value;

	i = Py_ReprEnter((PyObject *)mp);
	if (i != 0) {
		return i > 0 ? PyUnicode_FromString("{...}") : NULL;
	}

	if (mp->ma_used == 0) {
		result = PyUnicode_FromString("{}");
		goto Done;
	}

	pieces = PyList_New(0);
	if (pieces == NULL)
		goto Done;

	colon = PyUnicode_FromString(": ");
	if (colon == NULL)
		goto Done;

	/* Do repr() on each key+value pair, and insert ": " between them.
	   Note that repr may mutate the dict. */
	i = 0;
	while (PyDict_NextEx((PyObject *)mp, &i, &key, &value)) {
		int status;
		s = PyObject_Repr(key);
		PyUnicode_Append(&s, colon);
		PyUnicode_AppendAndDel(&s, PyObject_Repr(value));
		Py_DECREF(key);
		Py_DECREF(value);
		if (s == NULL)
			goto Done;
		status = PyList_Append(pieces, s);
		Py_DECREF(s);  /* append created a new ref */
		if (status < 0)
			goto Done;
	}

	/* Add "{}" decorations to the first and last items. */
	assert(PyList_GET_SIZE(pieces) > 0);
	s = PyUnicode_FromString("{");
	if (s == NULL)
		goto Done;
	temp = PyList_GET_ITEM(pieces, 0);
	PyUnicode_AppendAndDel(&s, temp);
	PyList_SET_ITEM(pieces, 0, s);
	if (s == NULL)
		goto Done;

	s = PyUnicode_FromString("}");
	if (s == NULL)
		goto Done;
	temp = PyList_GET_ITEM(pieces, PyList_GET_SIZE(pieces) - 1);
	PyUnicode_AppendAndDel(&temp, s);
	PyList_SET_ITEM(pieces, PyList_GET_SIZE(pieces) - 1, temp);
	if (temp == NULL)
		goto Done;

	/* Paste them all together with ", " between. */
	s = PyUnicode_FromString(", ");
	if (s == NULL)
		goto Done;
	result = PyUnicode_Join(s, pieces);
	Py_DECREF(s);

Done:
	Py_XDECREF(pieces);
	Py_XDECREF(colon);
	Py_ReprLeave((PyObject *)mp);
	return result;
}

static PyObject *
shareddict_repr(dictobject *mp)
{
	PyObject *s = NULL, *inner = NULL, *name = NULL, *format = NULL;
	PyObject *t = NULL;

	format = PyString_FromString("%s(%s)");
	if (format == NULL)
		goto Done;

	name = PyObject_GetAttrString((PyObject *)Py_Type(mp), "__name__");
	if (name == NULL)
		goto Done;

	inner = dict_repr(mp);
	if (inner == NULL)
		goto Done;

	t = PyTuple_New(2);
	if (t == NULL)
		goto Done;
	PyTuple_SET_ITEM(t, 0, name);
	name = NULL;
	PyTuple_SET_ITEM(t, 1, inner);
	inner = NULL;

	s = PyString_Format(format, t);

Done:
	Py_XDECREF(format);
	Py_XDECREF(name);
	Py_XDECREF(inner);
	Py_XDECREF(t);
	return s;
}

static Py_ssize_t
dict_length(dictobject *mp)
{
    Py_ssize_t len;
    PyDict_LockState lockstate;

    _pydictlock_initstate_read(&lockstate);
    _pydictlock_acquire(mp, &lockstate);
    len = mp->ma_used;
    _pydictlock_release(mp, &lockstate);

    return len;
}

static PyObject *
dict_subscript(dictobject *mp, register PyObject *key)
{
	PyObject *v;
	long hash;
	dictentry *ep;
	PyDict_LockState lockstate;

	if (!PyUnicode_CheckExact(key) ||
	    (hash = ((PyUnicodeObject *) key)->hash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return NULL;
	}

	_pydictlock_initstate_read(&lockstate);

	_pydictlock_acquire(mp, &lockstate);
	assert(mp->ma_table != NULL);
	ep = (mp->ma_lookup)(mp, key, hash, &lockstate);
	if (ep == NULL) {
		_pydictlock_release(mp, &lockstate);
		return NULL;
	}
	v = ep->me_value;
	if (v == NULL) {
		_pydictlock_release(mp, &lockstate);
		if (!PyDict_CheckExact(mp)) {
			/* Look up __missing__ method if we're a subclass. */
			PyObject *missing;
			static PyObject *missing_str = NULL;

			if (missing_str == NULL)
				missing_str =
				  PyUnicode_InternFromString("__missing__");

			if (_PyType_LookupEx(Py_Type(mp), missing_str, &missing) < 0)
				return NULL;
			if (missing != NULL) {
				v = PyObject_CallFunctionObjArgs(missing,
					(PyObject *)mp, key, NULL);
				Py_DECREF(missing);
				return v;
			}
		}
		set_key_error(key);
		return NULL;
	} else {
		Py_INCREF(v);
		_pydictlock_release(mp, &lockstate);
	}
	return v;
}

static int
dict_ass_sub(dictobject *mp, PyObject *v, PyObject *w)
{
	if (w == NULL)
		return PyDict_DelItem((PyObject *)mp, v);
	else
		return PyDict_SetItem((PyObject *)mp, v, w);
}

static PyMappingMethods dict_as_mapping = {
	(lenfunc)dict_length, /*mp_length*/
	(binaryfunc)dict_subscript, /*mp_subscript*/
	(objobjargproc)dict_ass_sub, /*mp_ass_subscript*/
};

static PyObject *
dict_keys(register dictobject *mp)
{
	register PyObject *v;
	register Py_ssize_t i, j;
	dictentry *ep;
	Py_ssize_t mask, n;
	PyDict_LockState lockstate;
	unsigned long long rebuilds;

  again:
	_pydictlock_initstate_read(&lockstate);

	_pydictlock_acquire(mp, &lockstate);
	n = mp->ma_used;
	rebuilds = mp->ma_rebuilds;
	_pydictlock_release(mp, &lockstate);
	v = PyList_New(n);
	if (v == NULL)
		return NULL;
	_pydictlock_acquire(mp, &lockstate);
	if (rebuilds != mp->ma_rebuilds) {
		_pydictlock_release(mp, &lockstate);
		/* Durnit.  The allocations caused the dict to resize.
		 * Just start over, this shouldn't normally happen.
		 */
		Py_DECREF(v);
		goto again;
	}
	assert(n == mp->ma_used);
	ep = mp->ma_table;
	mask = mp->ma_mask;
	for (i = 0, j = 0; i <= mask; i++) {
		if (ep[i].me_value != NULL) {
			PyObject *key = ep[i].me_key;
			Py_INCREF(key);
			PyList_SET_ITEM(v, j, key);
			j++;
		}
	}
	assert(j == n);
	_pydictlock_release(mp, &lockstate);
	return v;
}

static PyObject *
dict_values(register dictobject *mp)
{
	register PyObject *v;
	register Py_ssize_t i, j;
	dictentry *ep;
	Py_ssize_t mask, n;
	PyDict_LockState lockstate;
	unsigned long long rebuilds;

  again:
	_pydictlock_initstate_read(&lockstate);

	_pydictlock_acquire(mp, &lockstate);
	n = mp->ma_used;
	rebuilds = mp->ma_rebuilds;
	_pydictlock_release(mp, &lockstate);
	v = PyList_New(n);
	if (v == NULL)
		return NULL;
	_pydictlock_acquire(mp, &lockstate);
	if (rebuilds != mp->ma_rebuilds) {
		_pydictlock_release(mp, &lockstate);
		/* Durnit.  The allocations caused the dict to resize.
		 * Just start over, this shouldn't normally happen.
		 */
		Py_DECREF(v);
		goto again;
	}
	assert(n == mp->ma_used);
	ep = mp->ma_table;
	mask = mp->ma_mask;
	for (i = 0, j = 0; i <= mask; i++) {
		if (ep[i].me_value != NULL) {
			PyObject *value = ep[i].me_value;
			Py_INCREF(value);
			PyList_SET_ITEM(v, j, value);
			j++;
		}
	}
	assert(j == n);
	_pydictlock_release(mp, &lockstate);
	return v;
}

static PyObject *
dict_items(register dictobject *mp)
{
	register PyObject *v;
	register Py_ssize_t i, j, n;
	Py_ssize_t mask;
	PyObject *item, *key, *value;
	dictentry *ep;
	PyDict_LockState lockstate;
	unsigned long long rebuilds;

  again:
	_pydictlock_initstate_read(&lockstate);

	/* Preallocate the list of tuples, to avoid allocations during
	 * the loop over the items, which could trigger GC, which
	 * could resize the dict. :-(
	 */
	_pydictlock_acquire(mp, &lockstate);
	n = mp->ma_used;
	rebuilds = mp->ma_rebuilds;
	_pydictlock_release(mp, &lockstate);
	v = PyList_New(n);
	if (v == NULL)
		return NULL;
	for (i = 0; i < n; i++) {
		item = PyTuple_New(2);
		if (item == NULL) {
			Py_DECREF(v);
			return NULL;
		}
		PyList_SET_ITEM(v, i, item);
	}
	_pydictlock_acquire(mp, &lockstate);
	if (rebuilds != mp->ma_rebuilds) {
		_pydictlock_release(mp, &lockstate);
		/* Durnit.  The allocations caused the dict to resize.
		 * Just start over, this shouldn't normally happen.
		 */
		Py_DECREF(v);
		goto again;
	}
	/* Nothing we do below makes any function calls. */
	assert(n == mp->ma_used);
	ep = mp->ma_table;
	mask = mp->ma_mask;
	for (i = 0, j = 0; i <= mask; i++) {
		if ((value=ep[i].me_value) != NULL) {
			key = ep[i].me_key;
			item = PyList_GET_ITEM(v, j);
			Py_INCREF(key);
			PyTuple_SET_ITEM(item, 0, key);
			Py_INCREF(value);
			PyTuple_SET_ITEM(item, 1, value);
			j++;
		}
	}
	assert(j == n);
	_pydictlock_release(mp, &lockstate);
	return v;
}

static PyObject *
dict_fromkeys(PyObject *cls, PyObject *args)
{
	PyObject *seq;
	PyObject *value = Py_None;
	PyObject *it;	/* iter(seq) */
	PyObject *key;
	PyObject *d;
	int status;

	if (!PyArg_UnpackTuple(args, "fromkeys", 1, 2, &seq, &value))
		return NULL;

	assert(PyType_Check(cls));

	d = PyObject_CallObject(cls, NULL);
	if (d == NULL)
		return NULL;

	if (PyDict_CheckExact(d) && PyAnySet_CheckExact(seq)) {
		dictobject *mp = (dictobject *)d;
		Py_ssize_t pos = 0;
		PyObject *key;
		long hash;
		PyDict_LockState lockstate;

		_pydictlock_initstate_notshared(&lockstate);

		if (dictresize(mp, PySet_GET_SIZE(seq)))
			return NULL;

		while (_PySet_NextEntry(seq, &pos, &key, &hash)) {
			Py_INCREF(key);
			Py_INCREF(value);
			if (insertdict(mp, key, hash, value, &lockstate)) {
				Py_DECREF(d);
				return NULL;
			}
		}
		return d;
	}

	it = PyObject_GetIter(seq);
	if (it == NULL){
		Py_DECREF(d);
		return NULL;
	}

	for (;;) {
		key = PyIter_Next(it);
		if (key == NULL) {
			if (PyErr_Occurred())
				goto Fail;
			break;
		}
		status = PyObject_SetItem(d, key, value);
		Py_DECREF(key);
		if (status < 0)
			goto Fail;
	}

	Py_DECREF(it);
	return d;

Fail:
	Py_DECREF(it);
	Py_DECREF(d);
	return NULL;
}

static int
dict_update_common(PyObject *self, PyObject *args, PyObject *kwds,
		char *methname)
{
	PyObject *arg = NULL;
	int result = 0;

	if (!PyArg_UnpackTuple(args, methname, 0, 1, &arg))
		result = -1;

	else if (arg != NULL) {
		if (PyObject_HasAttrString(arg, "keys"))
			result = PyDict_Merge(self, arg, 1);
		else
			result = PyDict_MergeFromSeq2(self, arg, 1);
	}
	if (result == 0 && kwds != NULL)
		result = PyDict_Merge(self, kwds, 1);
	return result;
}

static PyObject *
dict_update(PyObject *self, PyObject *args, PyObject *kwds)
{
	if (dict_update_common(self, args, kwds, "update") != -1)
		Py_RETURN_NONE;
	return NULL;
}

/* Update unconditionally replaces existing items.
   Merge has a 3rd argument 'override'; if set, it acts like Update,
   otherwise it leaves existing items unchanged.

   PyDict_{Update,Merge} update/merge from a mapping object.

   PyDict_MergeFromSeq2 updates/merges from any iterable object
   that produces iterable objects of length 2.
*/

int
PyDict_MergeFromSeq2(PyObject *d, PyObject *seq2, int override)
{
	PyObject *it;	/* iter(seq2) */
	Py_ssize_t i;	/* index into seq2 of current element */
	PyObject *item;	/* seq2[i] */
	PyObject *fast;	/* item as a 2-tuple or 2-list */

	assert(d != NULL);
	assert(PyDict_Check(d));
	assert(seq2 != NULL);

	it = PyObject_GetIter(seq2);
	if (it == NULL)
		return -1;

	for (i = 0; ; ++i) {
		PyObject *key, *value;
		Py_ssize_t n;
		int status;

		fast = NULL;
		item = PyIter_Next(it);
		if (item == NULL) {
			if (PyErr_Occurred())
				goto Fail;
			break;
		}

		/* Convert item to sequence, and verify length 2. */
		fast = PySequence_Fast(item, "");
		if (fast == NULL) {
			if (PyErr_ExceptionMatches(PyExc_TypeError))
				PyErr_Format(PyExc_TypeError,
					"cannot convert dictionary update "
					"sequence element #%zd to a sequence",
					i);
			goto Fail;
		}
		n = PySequence_Fast_GET_SIZE(fast);
		if (n != 2) {
			PyErr_Format(PyExc_ValueError,
				     "dictionary update sequence element #%zd "
				     "has length %zd; 2 is required",
				     i, n);
			goto Fail;
		}

		/* Update/merge with this (key, value) pair. */
		key = PySequence_Fast_GET_ITEM(fast, 0);
		value = PySequence_Fast_GET_ITEM(fast, 1);
		if (override) {
			status = PyDict_SetItem(d, key, value);
			if (status < 0)
				goto Fail;
		} else if ((status = PyDict_Contains(d, key))) {
			if (status < 0)
				goto Fail;
			status = PyDict_SetItem(d, key, value);
			if (status < 0)
				goto Fail;
		}
		Py_DECREF(fast);
		Py_DECREF(item);
	}

	i = 0;
	goto Return;
Fail:
	Py_XDECREF(item);
	Py_XDECREF(fast);
	i = -1;
Return:
	Py_DECREF(it);
	return Py_SAFE_DOWNCAST(i, Py_ssize_t, int);
}

int
PyDict_Update(PyObject *a, PyObject *b)
{
	return PyDict_Merge(a, b, 1);
}

int
PyDict_Merge(PyObject *a, PyObject *b, int override)
{
	/* We accept for the argument either a concrete dictionary object,
	 * or an abstract "mapping" object.  For the former, we can do
	 * things quite efficiently.  For the latter, we only require that
	 * PyMapping_Keys() and PyObject_GetItem() be supported.
	 */
	if (a == NULL || !PyDict_Check(a) || b == NULL) {
		PyErr_BadInternalCall();
		return -1;
	}
	if (PyDict_CheckExact(b) && !PySharedDict_Check(a)) {
		register PyDictObject *mp = (PyDictObject *)a;
		register PyDictObject *other = (PyDictObject *)b;
		register Py_ssize_t i;
		dictentry *entry;
		PyDict_LockState lockstate;

		/* This branch is only used for normal dicts, so no
		 * locking is necessary */
		_pydictlock_initstate_notshared(&lockstate);

		if (other == mp || other->ma_used == 0)
			/* a.update(a) or a.update({}); nothing to do */
			return 0;
		if (mp->ma_used == 0)
			/* Since the target dict is empty, PyDict_GetItem()
			 * always returns NULL.  Setting override to 1
			 * skips the unnecessary test.
			 */
			override = 1;
		/* Do one big resize at the start, rather than
		 * incrementally resizing as we insert new items.  Expect
		 * that there will be no (or few) overlapping keys.
		 */
		if ((mp->ma_fill + other->ma_used)*3 >= (mp->ma_mask+1)*2) {
		   if (dictresize(mp, (mp->ma_used + other->ma_used)*2) != 0)
			   return -1;
		}
		for (i = 0; i <= other->ma_mask; i++) {
			entry = &other->ma_table[i];
			if (entry->me_value != NULL &&
			    (override ||
			     PyDict_GetItem(a, entry->me_key) == NULL)) {
				if (block_unshareable_keyvalue(a, entry->me_key,
						entry->me_value))
					return -1;
				Py_INCREF(entry->me_key);
				Py_INCREF(entry->me_value);
				if (insertdict(mp, entry->me_key,
					       (long)entry->me_hash,
					       entry->me_value, &lockstate) != 0)
					return -1;
			}
		}
	} else {
		/* Do it the generic, slower way */
		PyObject *keys = PyMapping_Keys(b);
		PyObject *iter;
		PyObject *key, *value;
		int status;

		if (keys == NULL)
			/* Docstring says this is equivalent to E.keys() so
			 * if E doesn't have a .keys() method we want
			 * AttributeError to percolate up.  Might as well
			 * do the same for any other error.
			 */
			return -1;

		iter = PyObject_GetIter(keys);
		Py_DECREF(keys);
		if (iter == NULL)
			return -1;

		for (key = PyIter_Next(iter); key; key = PyIter_Next(iter)) {
			if (!override && (status = PyDict_Contains(a, key))) {
				if (status < 0) {
					Py_DECREF(iter);
					return -1;
				}
				Py_DECREF(key);
				continue;
			}
			value = PyObject_GetItem(b, key);
			if (value == NULL) {
				Py_DECREF(iter);
				Py_DECREF(key);
				return -1;
			}
			status = PyDict_SetItem(a, key, value);
			Py_DECREF(key);
			Py_DECREF(value);
			if (status < 0) {
				Py_DECREF(iter);
				return -1;
			}
		}
		Py_DECREF(iter);
		if (PyErr_Occurred())
			/* Iterator completed, via error */
			return -1;
	}
	return 0;
}

static PyObject *
dict_copy(register dictobject *mp)
{
	return PyDict_Copy((PyObject*)mp);
}

PyObject *
PyDict_Copy(PyObject *o)
{
	PyObject *copy;

	if (o == NULL || !PyDict_Check(o)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	copy = PyDict_New();
	if (copy == NULL)
		return NULL;
	if (PyDict_Merge(copy, o, 1) == 0)
		return copy;
	Py_DECREF(copy);
	return NULL;
}

Py_ssize_t
PyDict_Size(PyObject *mp)
{
	if (mp == NULL || !PyDict_Check(mp)) {
		PyErr_BadInternalCall();
		return -1;
	}
	return dict_length((dictobject *)mp);
}

PyObject *
PyDict_Keys(PyObject *mp)
{
	if (mp == NULL || !PyDict_Check(mp)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	return dict_keys((dictobject *)mp);
}

PyObject *
PyDict_Values(PyObject *mp)
{
	if (mp == NULL || !PyDict_Check(mp)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	return dict_values((dictobject *)mp);
}

PyObject *
PyDict_Items(PyObject *mp)
{
	if (mp == NULL || !PyDict_Check(mp)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	return dict_items((dictobject *)mp);
}

/* Return 1 if dicts equal, 0 if not, -1 if error.
 * Gets out as soon as any difference is detected.
 * Uses only Py_EQ comparison.
 */
static int
dict_equal(PyObject *a, PyObject *b)
{
    PyObject *key, *value;
    Py_ssize_t i;

    if (PyDict_Size(a) != PyDict_Size(b))
        /* can't be equal if # of entries differ */
        return 0;

    /* Same # of entries -- check all of 'em.  Exit early on any diff. */
    i = 0;
    while (PyDict_NextEx(a, &i, &key, &value)) {
        PyObject *bvalue;
        int cmp;

        if (PyDict_GetItemEx(b, key, &bvalue) < 0) {
            Py_DECREF(key);
            Py_DECREF(value);
            return -1;
        }
        if (bvalue == NULL) {
            Py_DECREF(key);
            Py_DECREF(value);
            return 0;
        }

        cmp = PyObject_RichCompareBool(value, bvalue, Py_EQ);
        Py_DECREF(key);
        Py_DECREF(value);
        Py_DECREF(bvalue);
        if (cmp <= 0)  /* error or not equal */
            return cmp;
    }

    return 1;
 }

static PyObject *
dict_richcompare(PyObject *v, PyObject *w, int op)
{
	int cmp;
	PyObject *res;

	if (!PyDict_Check(v) || !PyDict_Check(w)) {
		res = Py_NotImplemented;
	}
	else if (op == Py_EQ || op == Py_NE) {
		cmp = dict_equal(v, w);
		if (cmp < 0)
			return NULL;
		res = (cmp == (op == Py_EQ)) ? Py_True : Py_False;
	}
	else
		res = Py_NotImplemented;
	Py_INCREF(res);
	return res;
 }

static PyObject *
dict_contains(register dictobject *mp, PyObject *key)
{
    PyObject *value;

    if (PyDict_GetItemEx((PyObject *)mp, key, &value) < 0)
        return NULL;

    if (value == NULL)
        return PyBool_FromLong(0);
    else {
        Py_DECREF(value);
        return PyBool_FromLong(1);
    }
}

static PyObject *
dict_get(register dictobject *mp, PyObject *args)
{
    PyObject *key;
    PyObject *failobj = Py_None;
    PyObject *val;

    if (!PyArg_UnpackTuple(args, "get", 1, 2, &key, &failobj))
        return NULL;

    if (PyDict_GetItemEx((PyObject *)mp, key, &val) < 0)
        return NULL;

    if (val == NULL) {
        Py_INCREF(failobj);
        return failobj;
    } else
        return val;
}


static PyObject *
dict_setdefault(register dictobject *mp, PyObject *args)
{
	PyObject *key;
	PyObject *failobj = Py_None;
	PyObject *val = NULL;
	long hash;
	dictentry *ep;
	PyDict_LockState lockstate;

	if (!PyArg_UnpackTuple(args, "setdefault", 1, 2, &key, &failobj))
		return NULL;

	if (!PyUnicode_CheckExact(key) ||
	    (hash = ((PyUnicodeObject *) key)->hash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return NULL;
	}
	if (block_unshareable_keyvalue((PyObject *)mp, key, val))
		return NULL;

	_pydictlock_initstate_write(&lockstate);

	_pydictlock_acquire(mp, &lockstate);
	ep = (mp->ma_lookup)(mp, key, hash, &lockstate);
	if (ep == NULL) {
		_pydictlock_release(mp, &lockstate);
		return NULL;
	}
	val = ep->me_value;
	if (val != NULL) {
		Py_INCREF(val);
		_pydictlock_release(mp, &lockstate);
	} else {
		_pydictlock_release(mp, &lockstate);
		/* XXX FIXME: This uses two separate operations, meaning
		 * it's not atomic.  This is wrong, but is it important
		 * enough to fix? */
		val = failobj;
		if (PyDict_SetItem((PyObject*)mp, key, failobj))
			val = NULL;
		else
			Py_INCREF(failobj);
	}
	return val;
}


static PyObject *
dict_clear(register dictobject *mp)
{
	PyDict_Clear((PyObject *)mp);
	Py_RETURN_NONE;
}

static PyObject *
dict_pop(dictobject *mp, PyObject *args)
{
	long hash;
	dictentry *ep;
	PyObject *old_value, *old_key;
	PyObject *key, *deflt = NULL;
	PyDict_LockState lockstate;

	if(!PyArg_UnpackTuple(args, "pop", 1, 2, &key, &deflt))
		return NULL;

	if (!PyUnicode_CheckExact(key) ||
	    (hash = ((PyUnicodeObject *) key)->hash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return NULL;
	}

	_pydictlock_initstate_write(&lockstate);

	_pydictlock_acquire(mp, &lockstate);
	if (mp->ma_used == 0) {
		_pydictlock_release(mp, &lockstate);
		if (deflt) {
			Py_INCREF(deflt);
			return deflt;
		}
		PyErr_SetString(PyExc_KeyError,
				"pop(): dictionary is empty");
		return NULL;
	}
	ep = (mp->ma_lookup)(mp, key, hash, &lockstate);
	if (ep == NULL) {
		_pydictlock_release(mp, &lockstate);
		return NULL;
	}
	if (ep->me_value == NULL) {
		_pydictlock_release(mp, &lockstate);
		if (deflt) {
			Py_INCREF(deflt);
			return deflt;
		}
		set_key_error(key);
		return NULL;
	}
	old_key = ep->me_key;
	Py_INCREF(dummy);
	ep->me_key = dummy;
	old_value = ep->me_value;
	ep->me_value = NULL;
	mp->ma_used--;
	_pydictlock_release(mp, &lockstate);
	Py_DECREF(old_key);
	return old_value;
}

static PyObject *
dict_popitem(dictobject *mp)
{
	Py_ssize_t i = 0;
	dictentry *ep;
	PyObject *res;
	PyDict_LockState lockstate;

	/* Allocate the result tuple before checking the size.  Believe it
	 * or not, this allocation could trigger a garbage collection which
	 * could empty the dict, so if we checked the size first and that
	 * happened, the result would be an infinite loop (searching for an
	 * entry that no longer exists).  Note that the usual popitem()
	 * idiom is "while d: k, v = d.popitem()". so needing to throw the
	 * tuple away if the dict *is* empty isn't a significant
	 * inefficiency -- possible, but unlikely in practice.
	 */
	res = PyTuple_New(2);
	if (res == NULL)
		return NULL;

	_pydictlock_initstate_write(&lockstate);

	_pydictlock_acquire(mp, &lockstate);
	if (mp->ma_used == 0) {
		_pydictlock_release(mp, &lockstate);
		Py_DECREF(res);
		PyErr_SetString(PyExc_KeyError,
				"popitem(): dictionary is empty");
		return NULL;
	}
	/* Set ep to "the first" dict entry with a value.  We abuse the hash
	 * field of slot 0 to hold a search finger:
	 * If slot 0 has a value, use slot 0.
	 * Else slot 0 is being used to hold a search finger,
	 * and we use its hash value as the first index to look.
	 */
	ep = &mp->ma_table[0];
	if (ep->me_value == NULL) {
		i = ep->me_hash;
		/* The hash field may be a real hash value, or it may be a
		 * legit search finger, or it may be a once-legit search
		 * finger that's out of bounds now because it wrapped around
		 * or the table shrunk -- simply make sure it's in bounds now.
		 */
		if (i > mp->ma_mask || i < 1)
			i = 1;	/* skip slot 0 */
		while ((ep = &mp->ma_table[i])->me_value == NULL) {
			i++;
			if (i > mp->ma_mask)
				i = 1;
		}
	}
	PyTuple_SET_ITEM(res, 0, ep->me_key);
	PyTuple_SET_ITEM(res, 1, ep->me_value);
	Py_INCREF(dummy);
	ep->me_key = dummy;
	ep->me_value = NULL;
	mp->ma_used--;
	assert(mp->ma_table[0].me_value == NULL);
	mp->ma_table[0].me_hash = i + 1;  /* next place to start */
	_pydictlock_release(mp, &lockstate);
	return res;
}

static int
dict_traverse(PyObject *op, visitproc visit, void *arg)
{
	Py_ssize_t i = 0;
	PyObject *pk;
	PyObject *pv;

	while (PyDict_Next(op, &i, &pk, &pv)) {
		Py_VISIT(pk);
		Py_VISIT(pv);
	}
	return 0;
}

static int
dict_tp_clear(PyObject *op)
{
	PyDict_Clear(op);
	return 0;
}


extern PyTypeObject PyDictIterKey_Type; /* Forward */
extern PyTypeObject PyDictIterValue_Type; /* Forward */
extern PyTypeObject PyDictIterItem_Type; /* Forward */
static PyObject *dictiter_new(dictobject *, PyTypeObject *);


PyDoc_STRVAR(contains__doc__,
"D.__contains__(k) -> True if D has a key k, else False");

PyDoc_STRVAR(getitem__doc__, "x.__getitem__(y) <==> x[y]");

PyDoc_STRVAR(get__doc__,
"D.get(k[,d]) -> D[k] if k in D, else d.  d defaults to None.");

PyDoc_STRVAR(setdefault_doc__,
"D.setdefault(k[,d]) -> D.get(k,d), also set D[k]=d if k not in D");

PyDoc_STRVAR(pop__doc__,
"D.pop(k[,d]) -> v, remove specified key and return the corresponding value\n\
If key is not found, d is returned if given, otherwise KeyError is raised");

PyDoc_STRVAR(popitem__doc__,
"D.popitem() -> (k, v), remove and return some (key, value) pair as a\n\
2-tuple; but raise KeyError if D is empty");

PyDoc_STRVAR(update__doc__,
"D.update(E, **F) -> None.  Update D from E and F: for k in E: D[k] = E[k]\
\n(if E has keys else: for (k, v) in E: D[k] = v) then: for k in F: D[k] = F[k]");

PyDoc_STRVAR(fromkeys__doc__,
"dict.fromkeys(S[,v]) -> New dict with keys from S and values equal to v.\n\
v defaults to None.");

PyDoc_STRVAR(clear__doc__,
"D.clear() -> None.  Remove all items from D.");

PyDoc_STRVAR(copy__doc__,
"D.copy() -> a shallow copy of D");

/* Forward */
static PyObject *dictkeys_new(PyObject *);
static PyObject *dictitems_new(PyObject *);
static PyObject *dictvalues_new(PyObject *);

PyDoc_STRVAR(keys__doc__,
	     "D.keys() -> a set-like object providing a view on D's keys");
PyDoc_STRVAR(items__doc__,
	     "D.items() -> a set-like object providing a view on D's items");
PyDoc_STRVAR(values__doc__,
	     "D.values() -> an object providing a view on D's values");

static PyMethodDef mapp_methods[] = {
	{"__contains__",(PyCFunction)dict_contains,     METH_O | METH_COEXIST,
	 contains__doc__},
	{"__getitem__", (PyCFunction)dict_subscript,	METH_O | METH_COEXIST,
	 getitem__doc__},
	{"get",         (PyCFunction)dict_get,          METH_VARARGS,
	 get__doc__},
	{"setdefault",  (PyCFunction)dict_setdefault,   METH_VARARGS,
	 setdefault_doc__},
	{"pop",         (PyCFunction)dict_pop,          METH_VARARGS,
	 pop__doc__},
	{"popitem",	(PyCFunction)dict_popitem,	METH_NOARGS,
	 popitem__doc__},
	{"keys",	(PyCFunction)dictkeys_new,	METH_NOARGS,
	keys__doc__},
	{"items",	(PyCFunction)dictitems_new,	METH_NOARGS,
	items__doc__},
	{"values",	(PyCFunction)dictvalues_new,	METH_NOARGS,
	values__doc__},
	{"update",	(PyCFunction)dict_update,	METH_VARARGS | METH_KEYWORDS,
	 update__doc__},
	{"fromkeys",	(PyCFunction)dict_fromkeys,	METH_VARARGS | METH_CLASS,
	 fromkeys__doc__},
	{"clear",	(PyCFunction)dict_clear,	METH_NOARGS,
	 clear__doc__},
	{"copy",	(PyCFunction)dict_copy,		METH_NOARGS,
	 copy__doc__},
	{NULL,		NULL}	/* sentinel */
};

/* Return 1 if `key` is in dict `op`, 0 if not, and -1 on error. */
int
PyDict_Contains(PyObject *op, PyObject *key)
{
	long hash;
	dictobject *mp = (dictobject *)op;
	dictentry *ep;
	int res;
	PyDict_LockState lockstate;

	if (!PyUnicode_CheckExact(key) ||
	    (hash = ((PyUnicodeObject *) key)->hash) == -1) {
		hash = PyObject_Hash(key);
		if (hash == -1)
			return -1;
	}

	_pydictlock_initstate_read(&lockstate);

	_pydictlock_acquire(mp, &lockstate);
	ep = (mp->ma_lookup)(mp, key, hash, &lockstate);
	res = ep == NULL ? -1 : (ep->me_value != NULL);
	_pydictlock_release(mp, &lockstate);
	return res;
}

/* Internal version of PyDict_Contains used when the hash value is already known */
int
_PyDict_Contains(PyObject *op, PyObject *key, long hash)
{
	dictobject *mp = (dictobject *)op;
	dictentry *ep;
	int res;
	PyDict_LockState lockstate;

	_pydictlock_initstate_read(&lockstate);

	_pydictlock_acquire(mp, &lockstate);
	ep = (mp->ma_lookup)(mp, key, hash, &lockstate);
	res = ep == NULL ? -1 : (ep->me_value != NULL);
	_pydictlock_release(mp, &lockstate);
	return res;
}

/* Hack to implement "key in dict" */
static PySequenceMethods dict_as_sequence = {
	0,			/* sq_length */
	0,			/* sq_concat */
	0,			/* sq_repeat */
	0,			/* sq_item */
	0,			/* sq_slice */
	0,			/* sq_ass_item */
	0,			/* sq_ass_slice */
	PyDict_Contains,	/* sq_contains */
	0,			/* sq_inplace_concat */
	0,			/* sq_inplace_repeat */
};

static PyObject *
dict_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	PyObject *self;

	assert(type != NULL);
	self = PyObject_New(type);
	if (self != NULL) {
		const size_t size = _PyObject_SIZE(type);
		PyDictObject *d = (PyDictObject *)self;
		INIT_NONZERO_DICT_SLOTS(d);
		d->ma_lookup = lookdict_unicode;
#ifdef SHOW_CONVERSION_COUNTS
		++created;
#endif
	}
	return self;
}

static PyObject *
shareddict_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PySharedDictObject *self = (PySharedDictObject *)dict_new(
        type, args, kwds);

    if (self == NULL)
        return NULL;

    self->readonly_mode = 0;
    self->read_count = 0;
    self->crit = PyCritical_Allocate(PyCRITICAL_NORMAL);
    if (self->crit == NULL) {
        Py_DECREF(self);
        PyErr_NoMemory();
        return NULL;
    }

    return (PyObject *)self;
}

static int
dict_init(PyObject *self, PyObject *args, PyObject *kwds)
{
	return dict_update_common(self, args, kwds, "dict");
}

static PyObject *
dict_iter(dictobject *dict)
{
	return dictiter_new(dict, &PyDictIterKey_Type);
}

static int
shareddict_isshareable (PyObject *self)
{
	return 1;
}

PyDoc_STRVAR(dictionary_doc,
"dict() -> new empty dictionary.\n"
"dict(mapping) -> new dictionary initialized from a mapping object's\n"
"    (key, value) pairs.\n"
"dict(seq) -> new dictionary initialized as if via:\n"
"    d = {}\n"
"    for k, v in seq:\n"
"        d[k] = v\n"
"dict(**kwargs) -> new dictionary initialized with the name=value pairs\n"
"    in the keyword argument list.  For example:  dict(one=1, two=2)");

PyTypeObject PyDict_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"dict",
	sizeof(dictobject),
	0,
	(destructor)dict_dealloc,		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	(reprfunc)dict_repr,			/* tp_repr */
	0,					/* tp_as_number */
	&dict_as_sequence,			/* tp_as_sequence */
	&dict_as_mapping,			/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
		Py_TPFLAGS_BASETYPE | Py_TPFLAGS_DICT_SUBCLASS |
		Py_TPFLAGS_SHAREABLE, /* tp_flags */
	dictionary_doc,				/* tp_doc */
	dict_traverse,				/* tp_traverse */
	dict_tp_clear,				/* tp_clear */
	dict_richcompare,			/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	(getiterfunc)dict_iter,			/* tp_iter */
	0,					/* tp_iternext */
	mapp_methods,				/* tp_methods */
	0,					/* tp_members */
	0,					/* tp_getset */
	0,					/* tp_base */
	0,					/* tp_dict */
	0,					/* tp_descr_get */
	0,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	dict_init,				/* tp_init */
	dict_new,				/* tp_new */
};

PyTypeObject PySharedDict_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"shareddict",
	sizeof(PySharedDictObject),
	0,
	(destructor)shareddict_dealloc,		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	(reprfunc)shareddict_repr,		/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	0,					/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
		Py_TPFLAGS_DICT_SUBCLASS |
		Py_TPFLAGS_SHAREABLE,		/* tp_flags */
	dictionary_doc,				/* tp_doc */
	dict_traverse,				/* tp_traverse */
	dict_tp_clear,				/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	0,					/* tp_iter */
	0,					/* tp_iternext */
	0,					/* tp_methods */
	0,					/* tp_members */
	0,					/* tp_getset */
	0,					/* tp_base */
	0,					/* tp_dict */
	0,					/* tp_descr_get */
	0,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	dict_init,				/* tp_init */
	shareddict_new,				/* tp_new */
	0,					/* tp_is_gc */
	0,					/* tp_bases */
	0,					/* tp_mro */
	0,					/* tp_cache */
	0,					/* tp_subclasses */
	0,					/* tp_weaklist */
	shareddict_isshareable,			/* tp_isshareable */
};

/* For backward compatibility with old dictionary interface */

PyObject *
PyDict_GetItemString(PyObject *v, const char *key)
{
	PyObject *kv, *rv;
	kv = PyUnicode_FromString(key);
	if (kv == NULL)
		return NULL;
	rv = PyDict_GetItem(v, kv);
	Py_DECREF(kv);
	return rv;
}

int
PyDict_GetItemStringEx(PyObject *d, const char *key, PyObject **value)
{
	PyObject *keyobj;
	int retvalue;
	keyobj = PyUnicode_FromString(key);
	if (keyobj == NULL)
		return -1;
	retvalue = PyDict_GetItemEx(d, keyobj, value);
	Py_DECREF(keyobj);
	return retvalue;
}

int
PyDict_SetItemString(PyObject *v, const char *key, PyObject *item)
{
	PyObject *kv;
	int err;
	kv = PyUnicode_FromString(key);
	if (kv == NULL)
		return -1;
	PyUnicode_InternInPlace(&kv); /* XXX Should we really? */
	err = PyDict_SetItem(v, kv, item);
	Py_DECREF(kv);
	return err;
}

int
PyDict_DelItemString(PyObject *v, const char *key)
{
	PyObject *kv;
	int err;
	kv = PyUnicode_FromString(key);
	if (kv == NULL)
		return -1;
	err = PyDict_DelItem(v, kv);
	Py_DECREF(kv);
	return err;
}

int
PyDict_ContainsString(PyObject *d, const char *key)
{
	PyObject *keyobj;
	int retvalue;
	keyobj = PyUnicode_FromString(key);
	if (keyobj == NULL)
		return -1;
	retvalue = PyDict_Contains(d, keyobj);
	Py_DECREF(keyobj);
	return retvalue;
}

/* Dictionary iterator types */

typedef struct {
	PyObject_HEAD
	dictobject *di_dict; /* Set to NULL when iterator is exhausted */
	Py_ssize_t di_used;
	Py_ssize_t di_pos;
	PyObject* di_result; /* reusable result tuple for iteritems */
	Py_ssize_t len;
} dictiterobject;

static PyObject *
dictiter_new(dictobject *dict, PyTypeObject *itertype)
{
	dictiterobject *di;
	di = PyObject_NEW(dictiterobject, itertype);
	if (di == NULL)
		return NULL;
	Py_INCREF(dict);
	di->di_dict = dict;
	di->di_used = dict_length(dict);
	di->di_pos = 0;
	di->len = di->di_used;
	if (itertype == &PyDictIterItem_Type) {
		di->di_result = PyTuple_Pack(2, Py_None, Py_None);
		if (di->di_result == NULL) {
			Py_DECREF(di);
			return NULL;
		}
	}
	else
		di->di_result = NULL;
	return (PyObject *)di;
}

static void
dictiter_dealloc(dictiterobject *di)
{
	Py_XDECREF(di->di_dict);
	Py_XDECREF(di->di_result);
	PyObject_DEL(di);
}

static PyObject *
dictiter_len(dictiterobject *di)
{
	Py_ssize_t len = 0;
	if (di->di_dict != NULL && di->di_used == dict_length(di->di_dict))
		len = di->len;
	return PyInt_FromSize_t(len);
}

PyDoc_STRVAR(length_hint_doc,
             "Private method returning an estimate of len(list(it)).");

static PyMethodDef dictiter_methods[] = {
	{"__length_hint__", (PyCFunction)dictiter_len, METH_NOARGS,
         length_hint_doc},
 	{NULL,		NULL}		/* sentinel */
};

/* On success, returns 0 and sets key and value (with NEW references)
 * On failure, returns -1, sets neither, and invalidates di */
static int
dictiter_iternext_common(dictiterobject *di, PyObject **key, PyObject **value)
{
    register Py_ssize_t i, mask;
    register dictentry *ep;
    dictobject *d = di->di_dict;
    PyDict_LockState lockstate;

    if (d == NULL)
        return -1;
    assert (PyDict_Check(d));

    _pydictlock_initstate_read(&lockstate);

    _pydictlock_acquire(d, &lockstate);
    /* We don't bother to check ma_rebuilds here.  We're not caching
     * ma_table or ma_mask, so ma_used is good enough. */
    if (di->di_used != d->ma_used) {
        _pydictlock_release(d, &lockstate);
        PyErr_SetString(PyExc_RuntimeError,
                        "dictionary changed size during iteration");
        di->di_used = -1; /* Make this state sticky */
        return -1;
    }

    i = di->di_pos;
    if (i < 0)
        goto fail;
    ep = d->ma_table;
    mask = d->ma_mask;
    while (i <= mask && ep[i].me_value == NULL)
        i++;
    di->di_pos = i+1;
    if (i > mask)
        goto fail;

    di->len--;
    *key = ep[i].me_key;
    *value = ep[i].me_value;
    Py_INCREF(*key);
    Py_INCREF(*value);

    _pydictlock_release(d, &lockstate);
    return 0;

fail:
    _pydictlock_release(d, &lockstate);
    Py_DECREF(d);
    di->di_dict = NULL;
    return -1;
}

static PyObject *dictiter_iternextkey(dictiterobject *di)
{
    PyObject *key, *value;

    if (dictiter_iternext_common(di, &key, &value))
        return NULL;
    Py_DECREF(value);
    return key;
}

PyTypeObject PyDictIterKey_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"dictionary-keyiterator",		/* tp_name */
	sizeof(dictiterobject),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)dictiter_dealloc, 		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
 	0,					/* tp_doc */
 	0,					/* tp_traverse */
 	0,					/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	PyObject_SelfIter,			/* tp_iter */
	(iternextfunc)dictiter_iternextkey,	/* tp_iternext */
	dictiter_methods,			/* tp_methods */
	0,
};

static PyObject *
dictiter_iternextvalue(dictiterobject *di)
{
    PyObject *key, *value;

    if (dictiter_iternext_common(di, &key, &value))
        return NULL;
    Py_DECREF(key);
    return value;
}

PyTypeObject PyDictIterValue_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"dictionary-valueiterator",		/* tp_name */
	sizeof(dictiterobject),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)dictiter_dealloc, 		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
 	0,					/* tp_doc */
 	0,					/* tp_traverse */
 	0,					/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	PyObject_SelfIter,			/* tp_iter */
	(iternextfunc)dictiter_iternextvalue,	/* tp_iternext */
	dictiter_methods,			/* tp_methods */
	0,
};

static PyObject *dictiter_iternextitem(dictiterobject *di)
{
    PyObject *key, *value;
    PyObject *result = di->di_result;

    if (dictiter_iternext_common(di, &key, &value))
        return NULL;

    if (Py_RefcntMatches(result, 1)) {
        Py_INCREF(result);
        Py_DECREF(PyTuple_GET_ITEM(result, 0));
        Py_DECREF(PyTuple_GET_ITEM(result, 1));
    } else {
        result = PyTuple_New(2);
        if (result == NULL)
            return NULL;
    }

    PyTuple_SET_ITEM(result, 0, key);
    PyTuple_SET_ITEM(result, 1, value);
    return result;
}

PyTypeObject PyDictIterItem_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"dictionary-itemiterator",		/* tp_name */
	sizeof(dictiterobject),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)dictiter_dealloc, 		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
 	0,					/* tp_doc */
 	0,					/* tp_traverse */
 	0,					/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	PyObject_SelfIter,			/* tp_iter */
	(iternextfunc)dictiter_iternextitem,	/* tp_iternext */
	dictiter_methods,			/* tp_methods */
	0,
};


/***********************************************/
/* View objects for keys(), items(), values(). */
/***********************************************/

/* The instance lay-out is the same for all three; but the type differs. */

typedef struct {
	PyObject_HEAD
	dictobject *dv_dict;
} dictviewobject;


static void
dictview_dealloc(dictviewobject *dv)
{
	Py_XDECREF(dv->dv_dict);
	PyObject_DEL(dv);
}

static Py_ssize_t
dictview_len(dictviewobject *dv)
{
	Py_ssize_t len = 0;
	if (dv->dv_dict != NULL)
		len = dict_length(dv->dv_dict);
	return len;
}

static PyObject *
dictview_new(PyObject *dict, PyTypeObject *type)
{
	dictviewobject *dv;
	if (dict == NULL) {
		PyErr_BadInternalCall();
		return NULL;
	}
	if (!PyDict_Check(dict)) {
		/* XXX Get rid of this restriction later */
		PyErr_Format(PyExc_TypeError,
			     "%s() requires a dict argument, not '%s'",
			     type->tp_name, dict->ob_type->tp_name);
		return NULL;
	}
	dv = PyObject_NEW(dictviewobject, type);
	if (dv == NULL)
		return NULL;
	Py_INCREF(dict);
	dv->dv_dict = (dictobject *)dict;
	return (PyObject *)dv;
}

/* TODO(guido): The views objects are not complete:

 * support more set operations
 * support arbitrary mappings?
   - either these should be static or exported in dictobject.h
   - if public then they should probably be in builtins
*/

/* Forward */
PyTypeObject PyDictKeys_Type;
PyTypeObject PyDictItems_Type;
PyTypeObject PyDictValues_Type;

#define PyDictKeys_Check(obj) ((obj)->ob_type == &PyDictKeys_Type)
#define PyDictItems_Check(obj) ((obj)->ob_type == &PyDictItems_Type)
#define PyDictValues_Check(obj) ((obj)->ob_type == &PyDictValues_Type)

/* This excludes Values, since they are not sets. */
# define PyDictViewSet_Check(obj) \
	(PyDictKeys_Check(obj) || PyDictItems_Check(obj))

/* Return 1 if self is a subset of other, iterating over self;
   0 if not; -1 if an error occurred. */
static int
all_contained_in(PyObject *self, PyObject *other)
{
	PyObject *iter = PyObject_GetIter(self);
	int ok = 1;

	if (iter == NULL)
		return -1;
	for (;;) {
		PyObject *next = PyIter_Next(iter);
		if (next == NULL) {
			if (PyErr_Occurred())
				ok = -1;
			break;
		}
		ok = PySequence_Contains(other, next);
		Py_DECREF(next);
		if (ok <= 0)
			break;
	}
	Py_DECREF(iter);
	return ok;
}

static PyObject *
dictview_richcompare(PyObject *self, PyObject *other, int op)
{
	Py_ssize_t len_self, len_other;
	int ok;
	PyObject *result;

	assert(self != NULL);
	assert(PyDictViewSet_Check(self));
	assert(other != NULL);

	if (!PyAnySet_Check(other) && !PyDictViewSet_Check(other)) {
		Py_INCREF(Py_NotImplemented);
		return Py_NotImplemented;
	}

	len_self = PyObject_Size(self);
	if (len_self < 0)
		return NULL;
	len_other = PyObject_Size(other);
	if (len_other < 0)
		return NULL;

	ok = 0;
	switch(op) {

	case Py_NE:
	case Py_EQ:
		if (len_self == len_other)
			ok = all_contained_in(self, other);
		if (op == Py_NE && ok >= 0)
			ok = !ok;
		break;

	case Py_LT:
		if (len_self < len_other)
			ok = all_contained_in(self, other);
		break;

	  case Py_LE:
		  if (len_self <= len_other)
			  ok = all_contained_in(self, other);
		  break;

	case Py_GT:
		if (len_self > len_other)
			ok = all_contained_in(other, self);
		break;

	case Py_GE:
		if (len_self >= len_other)
			ok = all_contained_in(other, self);
		break;

	}
	if (ok < 0)
		return NULL;
	result = ok ? Py_True : Py_False;
	Py_INCREF(result);
	return result;
}

/*** dict_keys ***/

static PyObject *
dictkeys_iter(dictviewobject *dv)
{
	if (dv->dv_dict == NULL) {
		Py_RETURN_NONE;
	}
	return dictiter_new(dv->dv_dict, &PyDictIterKey_Type);
}

static int
dictkeys_contains(dictviewobject *dv, PyObject *obj)
{
	if (dv->dv_dict == NULL)
		return 0;
	return PyDict_Contains((PyObject *)dv->dv_dict, obj);
}

static PySequenceMethods dictkeys_as_sequence = {
	(lenfunc)dictview_len,		/* sq_length */
	0,				/* sq_concat */
	0,				/* sq_repeat */
	0,				/* sq_item */
	0,				/* sq_slice */
	0,				/* sq_ass_item */
	0,				/* sq_ass_slice */
	(objobjproc)dictkeys_contains,	/* sq_contains */
};

static PyObject*
dictviews_sub(PyObject* self, PyObject *other)
{
	PyObject *result = PySet_New(self);
	PyObject *tmp;
	if (result == NULL)
		return NULL;

	tmp = PyObject_CallMethod(result, "difference_update", "O", other);
	if (tmp == NULL) {
		Py_DECREF(result);
		return NULL;
	}

	Py_DECREF(tmp);
	return result;
}

static PyObject*
dictviews_and(PyObject* self, PyObject *other)
{
	PyObject *result = PySet_New(self);
	PyObject *tmp;
	if (result == NULL)
		return NULL;

	tmp = PyObject_CallMethod(result, "intersection_update", "O", other);
	if (tmp == NULL) {
		Py_DECREF(result);
		return NULL;
	}

	Py_DECREF(tmp);
	return result;
}

static PyObject*
dictviews_or(PyObject* self, PyObject *other)
{
	PyObject *result = PySet_New(self);
	PyObject *tmp;
	if (result == NULL)
		return NULL;

	tmp = PyObject_CallMethod(result, "update", "O", other);
	if (tmp == NULL) {
		Py_DECREF(result);
		return NULL;
	}

	Py_DECREF(tmp);
	return result;
}

static PyObject*
dictviews_xor(PyObject* self, PyObject *other)
{
	PyObject *result = PySet_New(self);
	PyObject *tmp;
	if (result == NULL)
		return NULL;

	tmp = PyObject_CallMethod(result, "symmetric_difference_update", "O",
				  other);
	if (tmp == NULL) {
		Py_DECREF(result);
		return NULL;
	}

	Py_DECREF(tmp);
	return result;
}

static PyNumberMethods dictviews_as_number = {
	0,				/*nb_add*/
	(binaryfunc)dictviews_sub,	/*nb_subtract*/
	0,				/*nb_multiply*/
	0,				/*nb_remainder*/
	0,				/*nb_divmod*/
	0,				/*nb_power*/
	0,				/*nb_negative*/
	0,				/*nb_positive*/
	0,				/*nb_absolute*/
	0,				/*nb_bool*/
	0,				/*nb_invert*/
	0,				/*nb_lshift*/
	0,				/*nb_rshift*/
	(binaryfunc)dictviews_and,	/*nb_and*/
	(binaryfunc)dictviews_xor,	/*nb_xor*/
	(binaryfunc)dictviews_or,	/*nb_or*/
};

static PyMethodDef dictkeys_methods[] = {
 	{NULL,		NULL}		/* sentinel */
};

PyTypeObject PyDictKeys_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"dict_keys",				/* tp_name */
	sizeof(dictviewobject),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)dictview_dealloc, 		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	&dictviews_as_number,			/* tp_as_number */
	&dictkeys_as_sequence,			/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
 	0,					/* tp_doc */
 	0,					/* tp_traverse */
 	0,					/* tp_clear */
	dictview_richcompare,			/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	(getiterfunc)dictkeys_iter,		/* tp_iter */
	0,					/* tp_iternext */
	dictkeys_methods,			/* tp_methods */
	0,
};

static PyObject *
dictkeys_new(PyObject *dict)
{
	return dictview_new(dict, &PyDictKeys_Type);
}

/*** dict_items ***/

static PyObject *
dictitems_iter(dictviewobject *dv)
{
	if (dv->dv_dict == NULL) {
		Py_RETURN_NONE;
	}
	return dictiter_new(dv->dv_dict, &PyDictIterItem_Type);
}

static int
dictitems_contains(dictviewobject *dv, PyObject *obj)
{
	PyObject *key, *value, *found;
	int res;

	if (dv->dv_dict == NULL)
		return 0;
	if (!PyTuple_Check(obj) || PyTuple_GET_SIZE(obj) != 2)
		return 0;

	key = PyTuple_GET_ITEM(obj, 0);
	value = PyTuple_GET_ITEM(obj, 1);
	if (PyDict_GetItemEx((PyObject *)dv->dv_dict, key, &found) < 0)
		return -1;
	if (found == NULL)
		return 0;
	res = PyObject_RichCompareBool(value, found, Py_EQ);
	Py_DECREF(found);
	return res;
}

static PySequenceMethods dictitems_as_sequence = {
	(lenfunc)dictview_len,		/* sq_length */
	0,				/* sq_concat */
	0,				/* sq_repeat */
	0,				/* sq_item */
	0,				/* sq_slice */
	0,				/* sq_ass_item */
	0,				/* sq_ass_slice */
	(objobjproc)dictitems_contains,	/* sq_contains */
};

static PyMethodDef dictitems_methods[] = {
 	{NULL,		NULL}		/* sentinel */
};

PyTypeObject PyDictItems_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"dict_items",				/* tp_name */
	sizeof(dictviewobject),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)dictview_dealloc, 		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	&dictviews_as_number,			/* tp_as_number */
	&dictitems_as_sequence,			/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
 	0,					/* tp_doc */
 	0,					/* tp_traverse */
 	0,					/* tp_clear */
	dictview_richcompare,			/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	(getiterfunc)dictitems_iter,		/* tp_iter */
	0,					/* tp_iternext */
	dictitems_methods,			/* tp_methods */
	0,
};

static PyObject *
dictitems_new(PyObject *dict)
{
	return dictview_new(dict, &PyDictItems_Type);
}

/*** dict_values ***/

static PyObject *
dictvalues_iter(dictviewobject *dv)
{
	if (dv->dv_dict == NULL) {
		Py_RETURN_NONE;
	}
	return dictiter_new(dv->dv_dict, &PyDictIterValue_Type);
}

static PySequenceMethods dictvalues_as_sequence = {
	(lenfunc)dictview_len,		/* sq_length */
	0,				/* sq_concat */
	0,				/* sq_repeat */
	0,				/* sq_item */
	0,				/* sq_slice */
	0,				/* sq_ass_item */
	0,				/* sq_ass_slice */
	(objobjproc)0,			/* sq_contains */
};

static PyMethodDef dictvalues_methods[] = {
 	{NULL,		NULL}		/* sentinel */
};

PyTypeObject PyDictValues_Type = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	"dict_values",				/* tp_name */
	sizeof(dictviewobject),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)dictview_dealloc, 		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	&dictvalues_as_sequence,		/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
 	0,					/* tp_doc */
 	0,					/* tp_traverse */
 	0,					/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	(getiterfunc)dictvalues_iter,		/* tp_iter */
	0,					/* tp_iternext */
	dictvalues_methods,			/* tp_methods */
	0,
};

static PyObject *
dictvalues_new(PyObject *dict)
{
	return dictview_new(dict, &PyDictValues_Type);
}

/* Even type and object's initialization calls us, so we need a bare
 * minimum of functionality to be ready even before them. */
void
_PyDict_PreInit(void)
{
#ifdef USE_DICT_FREELIST
	free_dicts_lock = PyThread_lock_allocate();
	if (!free_dicts_lock)
		Py_FatalError("unable to allocate lock");
#endif
}

void
PyDict_Fini(void)
{
#ifdef USE_DICT_FREELIST
	PyThread_lock_free(free_dicts_lock);
	free_dicts_lock = NULL;
#endif
}
