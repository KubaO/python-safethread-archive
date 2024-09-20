/*

  Reference Cycle Garbage Collection
  ==================================

  Neil Schemenauer <nas@arctrix.com>

  Based on a post on the python-dev list.  Ideas from Guido van Rossum,
  Eric Tiedemann, and various others.

  http://www.arctrix.com/nas/python/gc/
  http://www.python.org/pipermail/python-dev/2000-March/003869.html
  http://www.python.org/pipermail/python-dev/2000-March/004010.html
  http://www.python.org/pipermail/python-dev/2000-March/004022.html

  For a highlevel view of the collection process, read the collect
  function.

*/

#include "Python.h"
#include "pythread.h"

#define GC_MAX_DEALLOC_DEPTH 50

#if 0
/* Get an object's GC head */
#define AS_GC(o) ((PyGC_Head *)(o)-1)

/* Get the object given the GC head */
#define FROM_GC(g) ((PyObject *)(((PyGC_Head *)g)+1))
#else
#define AS_GC
#define FROM_GC
#endif

/*** Global GC state ***/

struct gc_generation {
	PyGC_Head head;
	int threshold; /* collection threshold */
	int count;  /* For generation[0] the count is the number of new
		     * allocations.  For other generations count is the
		     * number of collections done of lower generations. */
};

#define NUM_GENERATIONS 3
#define GEN_HEAD(n) (&generations[n].head)

#if 0
/* linked lists of container objects */
static struct gc_generation generations[NUM_GENERATIONS] = {
	/* PyGC_Head,				threshold,	count */
	{{{0, GEN_HEAD(0), GEN_HEAD(0), 0}},	700,		0},
	{{{0, GEN_HEAD(1), GEN_HEAD(1), 0}},	10,		0},
	{{{0, GEN_HEAD(2), GEN_HEAD(2), 0}},	10,		0},
};
#else
/* linked lists of container objects */
static struct gc_generation generations[NUM_GENERATIONS] = {
	/* PyGC_Head,				threshold,	count */
	{{GEN_HEAD(0), GEN_HEAD(0), 0, 0, 0, 0, NULL},	700,		0},
	{{GEN_HEAD(1), GEN_HEAD(1), 0, 0, 0, 0, NULL},	10,		0},
	{{GEN_HEAD(2), GEN_HEAD(2), 0, 0, 0, 0, NULL},	10,		0},
};
#endif

PyGC_Head *_PyGC_generation0 = GEN_HEAD(0);

PyGC_Head trashcan = {&trashcan, &trashcan, 0, 0, 0, 0, NULL};

static int enabled = 1; /* automatic collection enabled? */

/* true if we are currently running the collector */
static int collecting = 0;

/* True if asynchronous refcounting has been used */
static AO_t gone_asynchronous = 0;

/* list of uncollectable objects */
static PyObject *garbage = NULL;

/* set for debugging information */
#define DEBUG_STATS		(1<<0) /* print collection statistics */
#define DEBUG_COLLECTABLE	(1<<1) /* print collectable objects */
#define DEBUG_UNCOLLECTABLE	(1<<2) /* print uncollectable objects */
#define DEBUG_SAVEALL		(1<<5) /* save all garbage in gc.garbage */
#define DEBUG_LEAK		DEBUG_COLLECTABLE | \
				DEBUG_UNCOLLECTABLE | \
				DEBUG_SAVEALL
static int debug;
static PyObject *tmod = NULL;

static PyThread_type_lock PyGC_lock;

/*--------------------------------------------------------------------------
gc_refs values.

Between collections, every gc'ed object has one of two gc_refs values:

GC_UNTRACKED
    The initial state; objects returned by PyObject_GC_Malloc are in this
    state.  The object doesn't live in any generation list, and
    gc_traverse must not be called.

GC_REACHABLE
    The object lives in some generation list, and gc_traverse is safe to
    call.  An object transitions to GC_REACHABLE when GC_Track
    is called.

During a collection, gc_refs can temporarily take on other states:

>= 0
    At the start of a collection, update_refs() copies the true refcount
    to gc_refs, for each object in the generation being collected.
    subtract_refs() then adjusts gc_refs so that it equals the number of
    times an object is referenced directly from outside the generation
    being collected.
    gc_refs remains >= 0 throughout these steps.

GC_TENTATIVELY_UNREACHABLE
    move_unreachable() then moves objects not reachable (whether directly or
    indirectly) from outside the generation into an "unreachable" set.
    Objects that are found to be reachable have gc_refs set to GC_REACHABLE
    again.  Objects that are found to be unreachable have gc_refs set to
    GC_TENTATIVELY_UNREACHABLE.  It's "tentatively" because the pass doing
    this can't be sure until it ends, and GC_TENTATIVELY_UNREACHABLE may
    transition back to GC_REACHABLE.

    Only objects with GC_TENTATIVELY_UNREACHABLE still set are candidates
    for collection.  If it's decided not to collect such an object (e.g.,
    it has a __del__ method), its gc_refs is restored to GC_REACHABLE again.
----------------------------------------------------------------------------
*/
#define GC_TRACKED              _PyGC_REFS_TRACKED
#define GC_TRACKED_YOUNG        _PyGC_REFS_TRACKED_YOUNG
#define GC_UNTRACKED            _PyGC_REFS_UNTRACKED
#define GC_UNTRACKED_YOUNG      _PyGC_REFS_UNTRACKED_YOUNG

static int
is_tracked(PyObject *o)
{
    if (o->ob_refcnt_trace == GC_TRACKED ||
            o->ob_refcnt_trace == GC_TRACKED_YOUNG)
        return 1;
    else if (o->ob_refcnt_trace == GC_UNTRACKED ||
            o->ob_refcnt_trace == GC_UNTRACKED_YOUNG)
        return 0;
    else
        Py_FatalError("is_tracked called on object in bad state");
}

static int
is_young(PyObject *o)
{
    if (o->ob_refcnt_trace == GC_TRACKED_YOUNG ||
            o->ob_refcnt_trace == GC_UNTRACKED_YOUNG)
        return 1;
    else if (o->ob_refcnt_trace == GC_TRACKED ||
            o->ob_refcnt_trace == GC_UNTRACKED)
        return 0;
    else
        Py_FatalError("is_young called on object in bad state");
}

static void
gc_track(PyObject *o)
{
    if (is_tracked(o))
        Py_FatalError("object already tracked");

    if (is_young(o))
        o->ob_refcnt_trace = GC_TRACKED_YOUNG;
    else
        o->ob_refcnt_trace = GC_TRACKED;
}

static void
gc_untrack(PyObject *o)
{
    if (!is_tracked(o))
        Py_FatalError("object already not tracked");

    if (is_young(o))
        o->ob_refcnt_trace = GC_UNTRACKED_YOUNG;
    else
        o->ob_refcnt_trace = GC_UNTRACKED;
}


/*** list functions ***/

static void
gc_list_init(PyGC_Head *list)
{
	list->ob_prev = list;
	list->ob_next = list;
}

static int
gc_list_is_empty(PyGC_Head *list)
{
	return (list->ob_next == list);
}

/* Append `node` to `list`. */
static void
gc_list_append(PyGC_Head *node, PyGC_Head *list)
{
	node->ob_next = list;
	node->ob_prev = list->ob_prev;
	node->ob_prev->ob_next = node;
	list->ob_prev = node;
}

/* Remove `node` from the gc list it's currently in. */
static void
gc_list_remove(PyGC_Head *node)
{
	node->ob_prev->ob_next = node->ob_next;
	node->ob_next->ob_prev = node->ob_prev;
	node->ob_next = NULL; /* object is not currently tracked */
}

/* Move `node` from the gc list it's currently in (which is not explicitly
 * named here) to the end of `list`.  This is semantically the same as
 * gc_list_remove(node) followed by gc_list_append(node, list).
 */
static void
gc_list_move(PyGC_Head *node, PyGC_Head *list)
{
	PyGC_Head *new_prev;
	PyGC_Head *current_prev = node->ob_prev;
	PyGC_Head *current_next = node->ob_next;
	/* Unlink from current list. */
	current_prev->ob_next = current_next;
	current_next->ob_prev = current_prev;
	/* Relink at end of new list. */
	new_prev = node->ob_prev = list->ob_prev;
	new_prev->ob_next = list->ob_prev = node;
	node->ob_next = list;
}

/* append list `from` onto list `to`; `from` becomes an empty list */
static void
gc_list_merge(PyGC_Head *from, PyGC_Head *to)
{
	PyGC_Head *tail;
	assert(from != to);
	if (!gc_list_is_empty(from)) {
		tail = to->ob_prev;
		tail->ob_next = from->ob_next;
		tail->ob_next->ob_prev = tail;
		to->ob_prev = from->ob_prev;
		to->ob_prev->ob_next = to;
	}
	gc_list_init(from);
}

static Py_ssize_t
gc_list_size(PyGC_Head *list)
{
	PyGC_Head *gc;
	Py_ssize_t n = 0;
	for (gc = list->ob_next; gc != list; gc = gc->ob_next) {
		n++;
	}
	return n;
}

/* Append objects in a GC list to a Python list.
 * Return 0 if all OK, < 0 if error (out of memory for list).
 */
static int
append_objects(PyObject *py_list, PyGC_Head *gc_list)
{
	PyGC_Head *gc;
	for (gc = gc_list->ob_next; gc != gc_list; gc = gc->ob_next) {
		PyObject *op = FROM_GC(gc);
		if (op != py_list) {
			if (PyList_Append(py_list, op)) {
				return -1; /* exception */
			}
		}
	}
	return 0;
}

/*** end of list stuff ***/


static void
gc_traverse(PyObject *ob, visitproc func, void *arg)
{
    traverseproc traverse;

    if (PyType_SUPPORTS_WEAKREFS(Py_TYPE(ob))) {
        PyWeakReference **ptr = _PY_GETWEAKREFPTR(ob);
        PyWeakReference *ref = (PyWeakReference *)AO_load_full((AO_t *)ptr);
        if (ref != NULL) {
            PyLinkedList *binding_links = &ref->binding_links;

            /* Traverse the weakref */
            if (func((PyObject *)ref, arg) != 0)
                Py_FatalError("non-zero retval in gc_traverse");

            while (PyLinkedList_Next(&ref->binding_links, &binding_links)) {
                PyWeakBinding *bind = PyLinkedList_Restore(PyWeakBinding,
                        weakref_links, binding_links);
                if (bind->value == NULL)
                    continue;

                /* Traverse a binding's value (which is really owned by ob) */
                if (func(bind->value, arg) != 0)
                    Py_FatalError("non-zero retval in gc_traverse");
            }

        }
    }

    /* Traverse the rest of ob */
    traverse = Py_TYPE(ob)->tp_traverse;
    if (traverse && traverse(ob, func, arg) != 0)
        Py_FatalError("non-zero retval in gc_traverse");
}

static void
gc_clear(PyObject *ob)
{
    inquiry clear = Py_TYPE(ob)->tp_clear;

    /* clear functions need a borrowed reference to ob.  Since they may
     * remove the reference that's currently in a cycle, we make sure
     * to create our own */
    Py_INCREF(ob);

    if (PyType_SUPPORTS_WEAKREFS(Py_TYPE(ob)))
        _PyObject_ForceClearWeakref(ob);

    if (clear != NULL)
        clear(ob);

    Py_DECREF(ob);
}


static void
flush_asynchronous(PyGC_Head *list)
{
    PyGC_Head asyncdelete;
    PyObject *ob, *next;

    gc_list_init(&asyncdelete);
    _PyState_FlushAsyncRefcounts();

    /* Separate objects with a refcount of 0 */
    for (ob = list->ob_next; ob != list; ob = next) {
        next = ob->ob_next;

        ob->ob_refowner = Py_REFOWNER_STATICINIT;

        if (ob->ob_refcnt == 0) {
            Py_INCREF(ob);
            gc_list_move(ob, &asyncdelete);
        }
    }

    /* Delete them */
    while (asyncdelete.ob_next != &asyncdelete) {
        ob = asyncdelete.ob_next;

        gc_list_move(ob, list);
        Py_DECREF(ob);
    }
}

static void
set_refcnt_trace(PyGC_Head *list, PyGC_Head *old)
{
    PyObject *ob, *next;

    for (ob = list->ob_next; ob != list; ob = next) {
        next = ob->ob_next;

        if (is_tracked(ob))
            ob->ob_refcnt_trace = ob->ob_refcnt;
        else
            gc_list_move(ob, old);
    }
}

/* A traversal callback for subtract_refs. */
static int
visit_decref(PyObject *ob, void *data)
{
    assert(ob != NULL);

    if (((Py_ssize_t)ob->ob_refcnt_trace) >= 0) {
        assert(ob->ob_refcnt_trace != 0);
        ob->ob_refcnt_trace--;
    }

    return 0;
}

/* Subtract internal references from gc_refs.  After this, gc_refs is >= 0
 * for all objects in containers, and is GC_REACHABLE for all tracked gc
 * objects not in containers.  The ones with gc_refs > 0 are directly
 * reachable from outside containers, and so can't be collected.
 */
static void
subtract_refs(PyGC_Head *list)
{
    PyGC_Head *ob;

    for (ob = list->ob_next; ob != list; ob = ob->ob_next)
        gc_traverse(ob, (visitproc)visit_decref, NULL);
}

/* A traversal callback for subtract_refs. */
static int
visit_incref(PyObject *ob, PyObject *templist)
{
    assert(ob != NULL);

    if (((Py_ssize_t)ob->ob_refcnt_trace) >= 0) {
        assert(ob->ob_refcnt_trace == 0);
        ob->ob_refcnt_trace = GC_TRACKED;
        gc_list_move(ob, templist);
    }

    return 0;
}

static Py_ssize_t
separate_unreachable(PyGC_Head *young, PyGC_Head *unreachable, PyGC_Head *old)
{
    Py_ssize_t m = 0;
    PyGC_Head temp;
    PyObject *ob, *next;

    gc_list_init(&temp);

    for (ob = young->ob_next; ob != young; ob = next) {
        next = ob->ob_next;

        if (((Py_ssize_t)ob->ob_refcnt_trace) < 0)
            gc_list_move(ob, old);
        else if (ob->ob_refcnt_trace > 0) {
            ob->ob_refcnt_trace = GC_TRACKED;
            gc_list_move(ob, &temp);
        }
    }

    while (temp.ob_next != &temp) {
        ob = temp.ob_next;
        gc_list_move(ob, old);

        gc_traverse(ob, (visitproc)visit_incref, &temp);
    }

    while (young->ob_next != young) {
        ob = young->ob_next;
        m += 1;
        ob->ob_refcnt_trace = GC_TRACKED;
        gc_list_move(ob, unreachable);
    }

    return m;
}

static void
clear_cyclic_objects(PyGC_Head *input, PyGC_Head *output)
{
    while (input->ob_next != input) {
        PyObject *ob = input->ob_next;

        gc_list_move(ob, output);
        gc_clear(ob);
    }
}

/* This is the main function.  Read this to understand how the
 * collection process works. */
/* Must be called with PyGC_lock held */
static Py_ssize_t
collect(int generation)
{
    int i;
    Py_ssize_t m = 0;  /* unreachable objects */
    PyGC_Head *young;
    PyGC_Head unreachable;
    PyGC_Head cleared;
    PyGC_Head old;

    PyThread_lock_release(PyGC_lock);
    PyState_StopTheWorld();

    //fprintf(stderr, "Collecting... ");

    /* update collection and allocation counters */
    if (generation+1 < NUM_GENERATIONS)
        generations[generation+1].count += 1;
    for (i = 0; i <= generation; i++)
        generations[i].count = 0;

    /* merge younger generations with one we are currently collecting */
    for (i = 0; i < generation; i++)
        gc_list_merge(GEN_HEAD(i), GEN_HEAD(generation));

    /* handy references */
    young = GEN_HEAD(generation);
    gc_list_init(&unreachable);
    gc_list_init(&cleared);
    gc_list_init(&old);

    /* Objects already on trashcan could have been revived through
     * a weakref, so we don't treat them special.  Objects added to
     * trashcan after this point are protected though, so we
     * require them to be deleted. */
    gc_list_merge(&trashcan, young);

    gone_asynchronous = 1;  /* Always do at least one pass */
    while (gone_asynchronous) {
        gone_asynchronous = 0;

        while (trashcan.ob_next != &trashcan)
            flush_asynchronous(&trashcan);

        flush_asynchronous(young);
    }
    assert(trashcan.ob_next == &trashcan);  /* Should be empty */

    // 4. scan generation, setting ob_refcnt_trace from ob_refcnt
    set_refcnt_trace(young, &old);
    // 5. call tp_trace to decrement ob_refcnt_trace
    subtract_refs(young);

    // 6. scan generation, doing:
    // 6a. moving unreachable objects to unreachable list
    // 6b. moving reachable objects to older generation list
    // 6c. resetting ob_refcnt_trace to GC_TRACKED
    m = separate_unreachable(young, &unreachable, &old);

    // 7. move unreachable list to cleared list, calling tp_clear while doing so
    clear_cyclic_objects(&unreachable, &cleared);

    // 8. assert cleared list becomes empty
    while (gone_asynchronous) {
        gone_asynchronous = 0;

        while (trashcan.ob_next != &trashcan)
            flush_asynchronous(&trashcan);

        flush_asynchronous(&cleared);
        flush_asynchronous(&old);
    }
    assert(trashcan.ob_next == &trashcan);  /* Should be empty */
    if (cleared.ob_next != &cleared) {
#if 1
        long i = 0;
        PyObject *obj = cleared.ob_next;
        while (obj != &cleared) {
            fprintf(stderr, "Uncollectable trash %ld:\n", i);
            _PyObject_Dump(obj);
            i++;
            obj = obj->ob_next;
        }
#else
        _PyObject_Dump(cleared.ob_next);
#endif
        Py_FatalError("Uncollectable trash");
    }

    // 9. merge old list with next-oldest generation
    if (generation < NUM_GENERATIONS-1)
        gc_list_merge(&old, GEN_HEAD(generation+1));
    else
        gc_list_merge(&old, GEN_HEAD(generation));

    if (PyErr_Occurred()) {
        PyObject *gc_str = PyUnicode_FromString("garbage collection");
        PyErr_WriteUnraisable(gc_str);
        Py_FatalError("unexpected exception during garbage collection");
    }

    PyState_StartTheWorld();
    PyThread_lock_acquire(PyGC_lock);

    //fprintf(stderr, "Done\n");

    return m;
}

static Py_ssize_t
collect_generations(void)
{
	int i;
	Py_ssize_t n = 0;

	/* Find the oldest generation (higest numbered) where the count
	 * exceeds the threshold.  Objects in the that generation and
	 * generations younger than it will be collected. */
	for (i = NUM_GENERATIONS-1; i >= 0; i--) {
		if (generations[i].count > generations[i].threshold) {
			n = collect(i);
			break;
		}
	}
	return n;
}

PyDoc_STRVAR(gc_enable__doc__,
"enable() -> None\n"
"\n"
"Enable automatic garbage collection.\n");

static PyObject *
gc_enable(PyObject *self, PyObject *noargs)
{
	PyThread_lock_acquire(PyGC_lock);
	enabled = 1;
	PyThread_lock_release(PyGC_lock);
	Py_INCREF(Py_None);
	return Py_None;
}

PyDoc_STRVAR(gc_disable__doc__,
"disable() -> None\n"
"\n"
"Disable automatic garbage collection.\n");

static PyObject *
gc_disable(PyObject *self, PyObject *noargs)
{
	PyThread_lock_acquire(PyGC_lock);
	enabled = 0;
	PyThread_lock_release(PyGC_lock);
	Py_INCREF(Py_None);
	return Py_None;
}

PyDoc_STRVAR(gc_isenabled__doc__,
"isenabled() -> status\n"
"\n"
"Returns true if automatic garbage collection is enabled.\n");

static PyObject *
gc_isenabled(PyObject *self, PyObject *noargs)
{
    int value;
    PyThread_lock_acquire(PyGC_lock);
    value = enabled;
    PyThread_lock_release(PyGC_lock);
    return PyBool_FromLong((long)value);
}

PyDoc_STRVAR(gc_collect__doc__,
"collect([generation]) -> n\n"
"\n"
"With no arguments, run a full collection.  The optional argument\n"
"may be an integer specifying which generation to collect.  A ValueError\n"
"is raised if the generation number is invalid.\n\n"
"The number of unreachable objects is returned.\n");

static PyObject *
gc_collect(PyObject *self, PyObject *args, PyObject *kws)
{
	static char *keywords[] = {"generation", NULL};
	int genarg = NUM_GENERATIONS - 1;
	Py_ssize_t n;

	if (!PyArg_ParseTupleAndKeywords(args, kws, "|i", keywords, &genarg))
		return NULL;

	else if (genarg < 0 || genarg >= NUM_GENERATIONS) {
		PyErr_SetString(PyExc_ValueError, "invalid generation");
		return NULL;
	}

	PyThread_lock_acquire(PyGC_lock);

	if (collecting)
		n = 0; /* already collecting, don't do anything */
	else {
		collecting = 1;
		n = collect(genarg);
		collecting = 0;
	}

	PyThread_lock_release(PyGC_lock);

	return PyLong_FromSsize_t(n);
}

PyDoc_STRVAR(gc_set_debug__doc__,
"set_debug(flags) -> None\n"
"\n"
"Set the garbage collection debugging flags. Debugging information is\n"
"written to sys.stderr.\n"
"\n"
"flags is an integer and can have the following bits turned on:\n"
"\n"
"  DEBUG_STATS - Print statistics during collection.\n"
"  DEBUG_COLLECTABLE - Print collectable objects found.\n"
"  DEBUG_UNCOLLECTABLE - Print unreachable but uncollectable objects found.\n"
"  DEBUG_SAVEALL - Save objects to gc.garbage rather than freeing them.\n"
"  DEBUG_LEAK - Debug leaking programs (everything but STATS).\n");

static PyObject *
gc_set_debug(PyObject *self, PyObject *args)
{
    int value;
    if (!PyArg_ParseTuple(args, "i:set_debug", &value))
        return NULL;

    PyThread_lock_acquire(PyGC_lock);
    debug = value;
    PyThread_lock_release(PyGC_lock);

    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(gc_get_debug__doc__,
"get_debug() -> flags\n"
"\n"
"Get the garbage collection debugging flags.\n");

static PyObject *
gc_get_debug(PyObject *self, PyObject *noargs)
{
    int value;
    PyThread_lock_acquire(PyGC_lock);
    value = debug;
    PyThread_lock_release(PyGC_lock);
    return Py_BuildValue("i", value);
}

PyDoc_STRVAR(gc_set_thresh__doc__,
"set_threshold(threshold0, [threshold1, threshold2]) -> None\n"
"\n"
"Sets the collection thresholds.  Setting threshold0 to zero disables\n"
"collection.\n");

static PyObject *
gc_set_thresh(PyObject *self, PyObject *args)
{
    int gens[3];
    int i;

    if (!PyArg_ParseTuple(args, "i|ii:set_threshold", &gens[0],
            &gens[1], &gens[2]))
        return NULL;

    PyThread_lock_acquire(PyGC_lock);
    generations[0].threshold = gens[0];
    if (PyTuple_GET_SIZE(args) > 1)
        generations[1].threshold = gens[1];
    if (PyTuple_GET_SIZE(args) > 2)
        generations[2].threshold = gens[2];
    for (i = 2; i < NUM_GENERATIONS; i++) {
        /* generations higher than 2 get the same threshold */
        generations[i].threshold = generations[2].threshold;
    }
    PyThread_lock_release(PyGC_lock);

    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(gc_get_thresh__doc__,
"get_threshold() -> (threshold0, threshold1, threshold2)\n"
"\n"
"Return the current collection thresholds\n");

static PyObject *
gc_get_thresh(PyObject *self, PyObject *noargs)
{
    int gens[3];

    PyThread_lock_acquire(PyGC_lock);
    gens[0] = generations[0].threshold;
    gens[1] = generations[1].threshold;
    gens[2] = generations[2].threshold;
    PyThread_lock_release(PyGC_lock);

    return Py_BuildValue("(iii)", gens[0], gens[1], gens[2]);
}

PyDoc_STRVAR(gc_get_count__doc__,
"get_count() -> (count0, count1, count2)\n"
"\n"
"Return the current collection counts\n");

static PyObject *
gc_get_count(PyObject *self, PyObject *noargs)
{
    int gens[3];

    PyThread_lock_acquire(PyGC_lock);
    gens[0] = generations[0].count;
    gens[1] = generations[1].count;
    gens[2] = generations[2].count;
    PyThread_lock_release(PyGC_lock);

    return Py_BuildValue("(iii)", gens[0], gens[1], gens[2]);
}

struct referrers_state {
    PyObject *objs;
    int match;
};

static int
referrersvisit(PyObject* obj, struct referrers_state *state)
{
	Py_ssize_t i;

	if (state->match)
		return 0;

	for (i = 0; i < PyTuple_GET_SIZE(state->objs); i++) {
		if (PyTuple_GET_ITEM(state->objs, i) == obj) {
			state->match = 1;
			return 0;
		}
	}

	return 0;
}

static int
gc_referrers_for(PyObject *objs, PyGC_Head *list, PyObject *resultlist)
{
	PyGC_Head *gc;
	PyObject *obj;
	struct referrers_state state;

	state.objs = objs;

	for (gc = list->ob_next; gc != list; gc = gc->ob_next) {
		obj = FROM_GC(gc);
		state.match = 0;

		gc_traverse(obj, (visitproc)referrersvisit, &state);
		if (state.match) {
			if (PyList_Append(resultlist, obj) < 0)
				return 0; /* error */
		}
	}
	return 1; /* no error */
}

PyDoc_STRVAR(gc_get_referrers__doc__,
"get_referrers(*objs) -> list\n\
Return the list of objects that directly refer to any of objs.");

static PyObject *
gc_get_referrers(PyObject *self, PyObject *args)
{
	int i;
	PyObject *result = PyList_New(0);
	if (!result)
		return NULL;

#warning gc_get_referrers needs updating to StopTheWorld
	for (i = 0; i < NUM_GENERATIONS; i++) {
		if (!(gc_referrers_for(args, GEN_HEAD(i), result))) {
			Py_DECREF(result);
			return NULL;
		}
	}
	return result;
}

struct referents_state {
    PyObject *list;
    int status;
};

/* Append obj to state->list; set state->status to 1 if an error (out
 * of memory) occurs. */
static int
referentsvisit(PyObject *obj, struct referents_state *state)
{
	if (state->status)
		return 0;

	if (PyList_Append(state->list, obj) < 0)
		state->status = 1;

	return 0;
}

PyDoc_STRVAR(gc_get_referents__doc__,
"get_referents(*objs) -> list\n\
Return the list of objects that are directly referred to by objs.");

static PyObject *
gc_get_referents(PyObject *self, PyObject *args)
{
	struct referents_state state;
	Py_ssize_t i;

	state.list = PyList_New(0);
	if (state.list == NULL)
		return NULL;

#warning gc_get_referents needs updating to StopTheWorld
	for (i = 0; i < PyTuple_GET_SIZE(args); i++) {
		traverseproc traverse;
		PyObject *obj = PyTuple_GET_ITEM(args, i);

		if (!PyObject_IS_GC(obj))
			continue;
		gc_traverse(obj, (visitproc)referentsvisit, &state);
		if (state.status) {
			Py_DECREF(state.list);
			return NULL;
		}
	}
	return state.list;
}

PyDoc_STRVAR(gc_get_objects__doc__,
"get_objects() -> [...]\n"
"\n"
"Return a list of objects tracked by the collector (excluding the list\n"
"returned).\n");

static PyObject *
gc_get_objects(PyObject *self, PyObject *noargs)
{
	int i;
	PyObject* result;

	result = PyList_New(0);
	if (result == NULL)
		return NULL;

#warning gc_get_objects needs updating to StopTheWorld
	for (i = 0; i < NUM_GENERATIONS; i++) {
		if (append_objects(result, GEN_HEAD(i))) {
			Py_DECREF(result);
			return NULL;
		}
	}
	return result;
}


PyDoc_STRVAR(gc__doc__,
"This module provides access to the garbage collector for reference cycles.\n"
"\n"
"enable() -- Enable automatic garbage collection.\n"
"disable() -- Disable automatic garbage collection.\n"
"isenabled() -- Returns true if automatic collection is enabled.\n"
"collect() -- Do a full collection right now.\n"
"get_count() -- Return the current collection counts.\n"
"set_debug() -- Set debugging flags.\n"
"get_debug() -- Get debugging flags.\n"
"set_threshold() -- Set the collection thresholds.\n"
"get_threshold() -- Return the current the collection thresholds.\n"
"get_objects() -- Return a list of all objects tracked by the collector.\n"
"get_referrers() -- Return the list of objects that refer to an object.\n"
"get_referents() -- Return the list of objects that an object refers to.\n");

static PyMethodDef GcMethods[] = {
	{"enable",	   gc_enable,	  METH_SHARED | METH_NOARGS,  gc_enable__doc__},
	{"disable",	   gc_disable,	  METH_SHARED | METH_NOARGS,  gc_disable__doc__},
	{"isenabled",	   gc_isenabled,  METH_SHARED | METH_NOARGS,  gc_isenabled__doc__},
	{"set_debug",	   gc_set_debug,  METH_SHARED | METH_VARARGS, gc_set_debug__doc__},
	{"get_debug",	   gc_get_debug,  METH_SHARED | METH_NOARGS,  gc_get_debug__doc__},
	{"get_count",	   gc_get_count,  METH_SHARED | METH_NOARGS,  gc_get_count__doc__},
	{"set_threshold",  gc_set_thresh, METH_SHARED | METH_VARARGS, gc_set_thresh__doc__},
	{"get_threshold",  gc_get_thresh, METH_SHARED | METH_NOARGS,  gc_get_thresh__doc__},
	{"collect",	   (PyCFunction)gc_collect,
         	METH_SHARED | METH_VARARGS | METH_KEYWORDS,           gc_collect__doc__},
	{"get_objects",    gc_get_objects,METH_SHARED | METH_NOARGS,  gc_get_objects__doc__},
	{"get_referrers",  gc_get_referrers, METH_SHARED | METH_VARARGS,
		gc_get_referrers__doc__},
	{"get_referents",  gc_get_referents, METH_SHARED | METH_VARARGS,
		gc_get_referents__doc__},
	{NULL,	NULL}		/* Sentinel */
};

void
_PyGC_Init(void)
{
	/* XXX we leak this */
	PyGC_lock = PyThread_lock_allocate();
	if (!PyGC_lock)
		Py_FatalError("unable to allocate lock");
}

PyMODINIT_FUNC
initgc(void)
{
	PyObject *m;

	m = Py_InitModule5("gc",
			      GcMethods,
			      gc__doc__,
			      NULL,
			      PYTHON_API_VERSION, 1);
	if (m == NULL)
		return;

#if 0
	if (garbage == NULL) {
		garbage = PyList_New(0);
		if (garbage == NULL)
			return;
	}
	Py_INCREF(garbage);
	if (PyModule_AddObject(m, "garbage", garbage) < 0)
		return;
#endif

	/* Importing can't be done in collect() because collect()
	 * can be called via PyGC_Collect() in Py_Finalize().
	 * This wouldn't be a problem, except that <initialized> is
	 * reset to 0 before calling collect which trips up
	 * the import and triggers an assertion.
	 */
	if (tmod == NULL) {
		tmod = PyImport_ImportModuleNoBlock("time");
		if (tmod == NULL)
			PyErr_Clear();
	}

#define ADD_INT(NAME) if (PyModule_AddIntConstant(m, #NAME, NAME) < 0) return
	ADD_INT(DEBUG_STATS);
	ADD_INT(DEBUG_COLLECTABLE);
	ADD_INT(DEBUG_UNCOLLECTABLE);
	ADD_INT(DEBUG_SAVEALL);
	ADD_INT(DEBUG_LEAK);
#undef ADD_INT
}

/* API to invoke gc.collect() from C */
Py_ssize_t
PyGC_Collect(void)
{
	Py_ssize_t n;

	PyThread_lock_acquire(PyGC_lock);

	if (collecting)
		n = 0; /* already collecting, don't do anything */
	else {
		collecting = 1;
		n = collect(NUM_GENERATIONS - 1);
		collecting = 0;
	}

	PyThread_lock_release(PyGC_lock);

	return n;
}

/* for debugging */
void
_PyGC_Dump(PyGC_Head *g)
{
	_PyObject_Dump(FROM_GC(g));
}

/* extension modules might be compiled with GC support so these
   functions must always be available */

#undef PyObject_GC_Del
#undef _PyObject_GC_Malloc

void
_Py_Refchain_Init(void)
{
#ifdef Py_TRACE_REFS
	refchain_lock = PyThread_lock_allocate();
	if (!refchain_lock)
		Py_FatalError("Can't allocate refchain_lock");
#endif
}

void
_Py_Refchain_Fini(void)
{
#ifdef Py_TRACE_REFS
	PyThread_lock_free(refchain_lock);
	refchain_lock = 0;
#endif
}

static void
_Py_NewReference(PyObject *op)
{
	_Py_INC_REFTOTAL();
	op->ob_refowner = (AO_t)PyState_Get();
	op->ob_refcnt = 1;
#ifdef Py_TRACE_REFS
	_Py_AddToAllObjects(op, 1);
#endif
	_Py_INC_TPALLOCS(op);
}

static void
_Py_ForgetReference(PyObject *op)
{
#ifdef Py_TRACE_REFS
#ifdef SLOW_UNREF_CHECK
        register PyObject *p;
#endif
	PyThread_lock_acquire(refchain_lock);
	if (Py_RefcntSnoop(op) < 0)
		Py_FatalError("UNREF negative refcnt");
	if (op == &refchain ||
	    op->_ob_prev->_ob_next != op || op->_ob_next->_ob_prev != op)
		Py_FatalError("UNREF invalid object");
#ifdef SLOW_UNREF_CHECK
	for (p = refchain._ob_next; p != &refchain; p = p->_ob_next) {
		if (p == op)
			break;
	}
	if (p == &refchain) /* Not found */
		Py_FatalError("UNREF unknown object");
#endif
	op->_ob_next->_ob_prev = op->_ob_prev;
	op->_ob_prev->_ob_next = op->_ob_next;
	op->_ob_next = op->_ob_prev = NULL;
#endif
	_Py_INC_TPFREES(op);
#ifdef Py_TRACE_REFS
	PyThread_lock_release(refchain_lock);
#endif
}

static void
_Py_Dealloc(register PyState *pystate, PyObject *op)
{
    destructor dealloc = Py_TYPE(op)->tp_dealloc;
    assert(dealloc != NULL);

    if (pystate->dealloc_depth > GC_MAX_DEALLOC_DEPTH) {
        PyThread_lock_acquire(PyGC_lock);
        if (is_young(op)) {
            if (is_tracked(op))
                op->ob_refcnt_trace = GC_TRACKED;
            else
                op->ob_refcnt_trace = GC_UNTRACKED;
        }
        gc_list_move(op, &trashcan);
        PyThread_lock_release(PyGC_lock);
        Py_DECREF_ASYNC(op);
        return;
    }

    if (PyType_IS_GC(Py_TYPE(op)))
        gc_untrack(op);

    if (PyType_SUPPORTS_WEAKREFS(Py_TYPE(op)) &&
            _PyObject_TryClearWeakref(op)) {
        /* He's not dead, he's pining for the fjords! */
        PyObject_Revive(op);
        Py_DECREF_ASYNC(op);
    } else {
        PyCritical dummycrit;

        _Py_INC_TPFREES(op) _Py_COUNT_ALLOCS_COMMA	\
        pystate->dealloc_depth++;

        if (pystate->critical_section == NULL ||
                pystate->critical_section->depth > PyCRITICAL_DEALLOC) {
            PyCritical_EnterDummy(&dummycrit, PyCRITICAL_DEALLOC);
            (*dealloc)(op);
            PyCritical_ExitDummy(&dummycrit);
        } else
            (*dealloc)(op);

        pystate->dealloc_depth--;

        if (pystate->dealloc_depth == 0) {
            /* XXX do something if we've got some queued deallocs */
        }
    }
}
#ifdef Py_TRACE_REFS
/* Print all live objects.  Because PyObject_Print is called, the
 * interpreter must be in a healthy state.
 */
void
_Py_PrintReferences(FILE *fp)
{
	PyObject *op;
	PyThread_lock_acquire(refchain_lock);
	fprintf(fp, "Remaining objects:\n");
	for (op = refchain._ob_next; op != &refchain; op = op->_ob_next) {
		fprintf(fp, "%p [%" PY_FORMAT_SIZE_T "d] ", op,
			Py_RefcntSnoop(op));
		/* XXX FIXME This is *wrong*.  It modifies the refchain
		   again to do the print. */
		if (PyObject_Print(op, fp, 0) != 0)
			PyErr_Clear();
		putc('\n', fp);
	}
	PyThread_lock_release(refchain_lock);
}

/* Print the addresses of all live objects.  Unlike _Py_PrintReferences, this
 * doesn't make any calls to the Python C API, so is always safe to call.
 */
void
_Py_PrintReferenceAddresses(FILE *fp)
{
	PyObject *op;
	PyThread_lock_acquire(refchain_lock);
	fprintf(fp, "Remaining object addresses:\n");
	for (op = refchain._ob_next; op != &refchain; op = op->_ob_next)
		fprintf(fp, "%p [%" PY_FORMAT_SIZE_T "d] %s\n", op,
			Py_RefcntSnoop(op), Py_TYPE(op)->tp_name);
	PyThread_lock_release(refchain_lock);
}

/* This is dangerous.  It relies on PyList_Append to not create or
 * delete any objects. */
PyObject *
_Py_GetObjects(PyObject *self, PyObject *args)
{
	int i, n;
	PyObject *t = NULL;
	PyObject *res, *op;

	if (!PyArg_ParseTuple(args, "i|O", &n, &t))
		return NULL;
	res = PyList_New(0);
	if (res == NULL)
		return NULL;
	PyThread_lock_acquire(refchain_lock);
	op = refchain._ob_next;
	for (i = 0; (n == 0 || i < n) && op != &refchain; i++) {
		while (op == self || op == args || op == res || op == t ||
		       (t != NULL && Py_TYPE(op) != (PyTypeObject *) t)) {
			op = op->_ob_next;
			if (op == &refchain) {
				PyThread_lock_release(refchain_lock);
				return res;
			}
		}
		if (PyList_Append(res, op) < 0) {
			PyThread_lock_release(refchain_lock);
			Py_DECREF(res);
			return NULL;
		}
		op = op->_ob_next;
	}
	PyThread_lock_release(refchain_lock);
	return res;
}

#endif
static AO_t hit_count;
static AO_t adj_count;
static AO_T col_count;

static void
add_hit(void)
{
	AO_t count = AO_fetch_and_add1(&hit_count);
	if ((count % 1000000) == 0)
		printf("Hits: %lu\n", count);
}

static void
add_adj(void)
{
	AO_t count = AO_fetch_and_add1(&adj_count);
	if ((count % 1000) == 0)
		printf("Adjacent: %lu\n", count);
}

static void
add_col(void)
{
	AO_t count = AO_fetch_and_add1(&col_count);
	if ((count % 1000000) == 0)
		printf("Collisions: %lu\n", count);
}

static AO_T obj_count;

static void
add_obj(void)
{
	AO_t count = AO_fetch_and_add1(&obj_count);
	if ((count % 1000) == 0)
		printf("Objects: %lu\n", count);
}

static void
del_obj(void)
{
	AO_fetch_and_sub1(&obj_count);
}


static inline void
_PyGC_AsyncRefcount_FlushSingle(PyAsyncRefEntry *entry)
{
	assert(entry->obj);
	AO_fetch_and_add_full(&entry->obj->ob_refcnt, entry->diff);
	entry->obj = NULL;
	entry->diff = 0;
}

void
_PyGC_AsyncRefcount_Flush(PyState *pystate)
{
	int i;

	for (i = 0; i < Py_ASYNCREFCOUNT_TABLE; i++) {
		PyAsyncRefEntry *entry = &pystate->async_refcounts[i];
		if (entry->obj != NULL)
			_PyGC_AsyncRefcount_FlushSingle(entry);
		assert(entry->obj == NULL);
		assert(entry->diff == 0);
	}
}

/* Attempts to promote the object's refowner one step.  May fail, even
 * allowing the object's refowner to change to something else entierly. */
void
_PyGC_RefMode_Promote(PyObject *op)
{
	PyState *pystate = PyState_Get();
	AO_t oldmode;

	//assert(monitorspace != NULL);
	//printf("Promoting %p\n", op);

	oldmode = AO_load_acquire(&op->ob_refowner);
	if (oldmode == Py_REFOWNER_STATICINIT)
		AO_compare_and_swap_full(&op->ob_refowner,
			Py_REFOWNER_STATICINIT, (AO_t)pystate);
	else if (oldmode == Py_REFOWNER_ASYNC) {
		/* Do nothing */
	} else {
		/* XXX FIXME this should only be a partial suspend.  We
		 * musn't allow the tracing GC to collect the
		 * PyState we're about to use.  Some sort of usage
		 * count? */
		PyState *owner = (PyState *)oldmode;
		PyCritical dummycrit;
		PyCritical_EnterDummy(&dummycrit, PyCRITICAL_REFMODE_PROMOTE);

		PyState_MaybeSuspend();
		PyThread_lock_acquire(owner->refowner_waiting_lock);
		AO_store_full(&owner->refowner_waiting_flag, 1);
		PyThread_lock_acquire(owner->refowner_lock);
		AO_store_full(&owner->refowner_waiting_flag, 0);
		PyThread_lock_release(owner->refowner_waiting_lock);

		/* Another thread may already have altered the object's
		 * refowner field, so we do another comparison. */
		AO_compare_and_swap_full(&op->ob_refowner, oldmode,
			Py_REFOWNER_ASYNC);

		if (AO_load_acquire(&gone_asynchronous) == 0)
			AO_store_full(&gone_asynchronous, 1);

		PyThread_lock_release(owner->refowner_lock);
		PyState_MaybeResume();

		PyCritical_ExitDummy(&dummycrit);
	}
}

static PyAsyncRefEntry *
_Py_GetAsyncRefEntry(PyState *pystate, PyObject *op)
{
	/* XXX this probably needs to be heavily optimized */
	PyAsyncRefEntry *entry;
	AO_t index = (AO_t)op;

	index ^= (index >> 3) ^ (index >> 7) ^ (index >> 17);
	index &= Py_ASYNCREFCOUNT_TABLE - 1;

	entry = &pystate->async_refcounts[index];

	if (entry->obj == op || entry->obj == NULL) {
		//add_hit();
		return entry;
	}
	//add_col();
	_PyGC_AsyncRefcount_FlushSingle(entry);
	return entry;
}


void
Py_IncRef(PyObject *o)
{
    Py_XINCREF(o);
}

void
Py_DecRef(PyObject *o)
{
    Py_XDECREF(o);
}

/* These seem to help on my box, but on other boxes or different compiler
 * versions may produce too strong of a preference.  YMMV. */
#define _Py_EXPECT(expr) __builtin_expect((expr) != 0, 1)
#define _Py_NOEXPECT(expr) __builtin_expect((expr) != 0, 0)

#ifdef WITH_FREETHREAD
void
_Py_INCREF(PyObject *op, register PyState *pystate)
{
	assert(pystate != NULL);
	assert(!pystate->suspended);

//	if (PyType_Check(op) && ((PyTypeObject *)op)->tp_flags & Py_TPFLAGS_HEAPTYPE)
//		printf("Heap type incref %s %d\n",
//			((PyTypeObject *)op)->tp_name, Py_RefcntSnoop(op));

	_Py_INC_REFTOTAL();

	/* Blah, this should be done at compile time, or maybe in configure. */
	assert(sizeof(AO_t) == sizeof(Py_ssize_t));

	while (1) {

		void *owner = (void *)AO_load_acquire(&op->ob_refowner);
		if (_Py_EXPECT(owner == Py_REFOWNER_ASYNC)) {
			/* This should use a pystate hash table */
			PyAsyncRefEntry *entry = _Py_GetAsyncRefEntry(pystate, op);
			entry->diff++;
			if (entry->diff == 0)
				entry->obj = NULL;
			else
				entry->obj = op;
			return;
		} else if (_Py_EXPECT(owner == pystate)) {
			op->ob_refcnt++;
			return;
		} else {
			_PyGC_RefMode_Promote(op);
			continue;
		}
	}
}

void
_Py_DECREF(PyObject *op, register PyState *pystate)
{
	assert(pystate != NULL);
	assert(!pystate->suspended);

//	if (PyType_Check(op) && ((PyTypeObject *)op)->tp_flags & Py_TPFLAGS_HEAPTYPE)
//		printf("Heap type decref %s %d\n",
//			((PyTypeObject *)op)->tp_name, Py_RefcntSnoop(op));

	_Py_DEC_REFTOTAL();

	/* Blah, this should be done at compile time, or maybe in configure. */
	assert(sizeof(AO_t) == sizeof(Py_ssize_t));

	while (1) {
		void *owner = (void *)AO_load_acquire(&op->ob_refowner);
		if (_Py_EXPECT(owner == Py_REFOWNER_ASYNC)) {
			PyAsyncRefEntry *entry = _Py_GetAsyncRefEntry(pystate, op);
			entry->diff--;
			if (entry->diff == 0)
				entry->obj = NULL;
			else
				entry->obj = op;
			return;
		} else if (_Py_EXPECT(owner == pystate)) {
			if (op->ob_refcnt > 1)
				op->ob_refcnt--;
			else
				_Py_Dealloc(pystate, op);
#ifdef Py_REF_DEBUG
			if (((Py_ssize_t)op->ob_refcnt) < 0)
				_Py_NegativeRefcount(__FILE__, __LINE__, op, op->ob_refcnt);
#endif
			return;
		} else {
			_PyGC_RefMode_Promote(op);
			continue;
		}
	}
}

/* Ensures the DECREF is always asynchronous, and thus will not
 * recursively call _Py_Dealloc */
void
_Py_DECREF_ASYNC(PyObject *op, register PyState *pystate)
{
	assert(pystate != NULL);
	assert(!pystate->suspended);

//	if (PyType_Check(op) && ((PyTypeObject *)op)->tp_flags & Py_TPFLAGS_HEAPTYPE)
//		printf("Heap type decref %s %d\n",
//			((PyTypeObject *)op)->tp_name, Py_RefcntSnoop(op));

	_Py_DEC_REFTOTAL();

	/* Blah, this should be done at compile time, or maybe in configure. */
	assert(sizeof(AO_t) == sizeof(Py_ssize_t));

	while (1) {
		void *owner = (void *)AO_load_acquire(&op->ob_refowner);
		if (_Py_EXPECT(owner == Py_REFOWNER_ASYNC)) {
			PyAsyncRefEntry *entry = _Py_GetAsyncRefEntry(pystate, op);
			entry->diff--;
			if (entry->diff == 0)
				entry->obj = NULL;
			else
				entry->obj = op;
			return;
		} else {
			_PyGC_RefMode_Promote(op);
			continue;
		}
	}
}
#endif /* WITH_FREETHREAD */

Py_ssize_t
_Py_RefcntSnoop(PyObject *op)
{
    PyState *pystate = PyState_Get();

    while (1) {
        void *owner = (void *)AO_load_acquire(&op->ob_refowner);

        if (owner == pystate)
            return op->ob_refcnt;
        else if (owner == (void *)Py_REFOWNER_STATICINIT)
            _PyGC_RefMode_Promote(op);
        else
            return 1000000;  /* Arbitrary large value */
    }
}


#define GET_SIZE(size_class) ((size_class) <= 0 ? gc_cache_size_classes[-(size_class)] : (size_class))

/* XXX Must match up with PYGC_CACHE_SIZECLASSES */
static const Py_ssize_t gc_cache_size_classes[] = {
	32,
	48,
	64,
	96,
	128,
	192,
	256,
	384,
	512,
	768,
	1024,
	1536,
	2048,
};

static Py_ssize_t
find_size_class(size_t size)
{
	Py_ssize_t i;

	assert(sizeof(gc_cache_size_classes) / sizeof(*gc_cache_size_classes) ==
			PYGC_CACHE_SIZECLASSES);
	if (size > gc_cache_size_classes[PYGC_CACHE_SIZECLASSES - 1])
		return size; /* Too large to cache */

	for (i = 0; ; i++) {
		if (size <= gc_cache_size_classes[i])
			return -i;
	}
}


static void
PyGC_lock_count(void)
{
#if 0
	static unsigned long long count;
	count++;
	if ((count % 1000) == 0)
		printf("Lock count: %llu\n", count);
#endif
}

static PyObject *
_PyObject_GC_Malloc(size_t basicsize)
{
	PyGC_Head *g = NULL;
	/* XXX FIXME unsigned -> signed overflow? */
	//Py_ssize_t size_class = find_size_class(sizeof(PyGC_Head) + basicsize);
	Py_ssize_t size_class = find_size_class(basicsize);

	if (size_class <= 0) {
		PyState *pystate = PyState_Get();
		Py_ssize_t i;

		for (i = 0; i < PYGC_CACHE_COUNT; i++) {
			if (pystate->gc_object_cache[-size_class][i] != NULL) {
				g = pystate->gc_object_cache[-size_class][i];
				pystate->gc_object_cache[-size_class][i] = NULL;
				g->ob_sizeclass = size_class;
				assert(!is_tracked(g));
				//printf("Cache hit!\n");
				break;
			}
		}
	}

	if (g == NULL) {
		//printf("Cache miss.\n");
		g = malloc(GET_SIZE(size_class));
		if (g == NULL)
			return PyErr_NoMemory();
		g->ob_sizeclass = size_class;
		g->ob_refcnt_trace = GC_UNTRACKED_YOUNG;

		PyThread_lock_acquire(PyGC_lock);
		PyGC_lock_count();

		generations[0].count++; /* number of allocated GC objects */
		if (generations[0].count > generations[0].threshold &&
				enabled &&
				generations[0].threshold &&
				!collecting &&
				!PyErr_Occurred()) {
			collecting = 1;
			collect_generations();
			collecting = 0;
		}

		gc_list_append(g, _PyGC_generation0);

		PyThread_lock_release(PyGC_lock);
	}

	return FROM_GC(g);
}

static PyVarObject *
_PyObject_GC_Resize(PyVarObject *op, Py_ssize_t nitems)
{
	/* XXX any overflow possible? */
	/* XXX FIXME Some code assumes a sentinal is allocated.  Blah. */
	const size_t basicsize = _PyObject_VAR_SIZE(Py_TYPE(op), nitems + 1);
	PyObject *g = (PyObject *)op;
	//Py_ssize_t size_class = find_size_class(sizeof(PyGC_Head) + basicsize);
	Py_ssize_t size_class = find_size_class(basicsize);

	if (is_tracked((PyObject *)op))
		Py_FatalError("_PyObject_GC_Resize called for tracked object");

	if (size_class == g->ob_sizeclass) {
		//printf("Resize avoided\n");
		Py_SIZE(op) = nitems;
		return op; /* That was easy */
	}

	//printf("Resizing\n");
	PyThread_lock_acquire(PyGC_lock);

	g = realloc(g, GET_SIZE(size_class));
	if (g == NULL) {
		PyThread_lock_release(PyGC_lock);
		return (PyVarObject *) PyErr_NoMemory();
	}

	g->ob_sizeclass = size_class;
	gc_list_move(g, _PyGC_generation0);

	PyThread_lock_release(PyGC_lock);

	op = (PyVarObject *) FROM_GC(g);
	Py_SIZE(op) = nitems;
	return op;
}

static void
_PyObject_GC_Del(void *arg)
{
	PyGC_Head *g = AS_GC(arg);
	Py_ssize_t size_class = g->ob_sizeclass;

	assert(g == arg); /* WTF? */

	if (size_class <= 0 && is_young(g)) {
		PyState *pystate = PyState_Get();
		Py_ssize_t i;

		for (i = 0; i < PYGC_CACHE_COUNT; i++) {
			if (pystate->gc_object_cache[-size_class][i] == NULL) {
				//printf("Filling cache\n");
				pystate->gc_object_cache[-size_class][i] = g;
				return;
			}
		}
	}
	//printf("Cache full\n");

	PyThread_lock_acquire(PyGC_lock);
	PyGC_lock_count();

	gc_list_remove(g);
	if (generations[0].count > 0) {
		generations[0].count--;
	}

	PyThread_lock_release(PyGC_lock);
	free(g);
}

void
_PyGC_Object_Cache_Flush(void)
{
	PyState *pystate = PyState_Get();
	Py_ssize_t i, j;

	PyThread_lock_acquire(PyGC_lock);
	PyGC_lock_count();

	for (i = 0; i < PYGC_CACHE_SIZECLASSES; i++) {
		for (j = 0; j < PYGC_CACHE_COUNT; j++) {
			PyGC_Head *g = pystate->gc_object_cache[i][j];
			pystate->gc_object_cache[i][j] = NULL;

			if (g != NULL) {
				gc_list_remove(g);
				if (generations[0].count > 0) {
					generations[0].count--;
				}

				assert(g->ob_refcnt == Py_REFCNT_DELETED);

				free(g);
			}
		}
	}

	PyThread_lock_release(PyGC_lock);
}

PyObject *
_PyObject_New(PyTypeObject *tp)
{
	const size_t size = _PyObject_SIZE(tp);
	PyObject *op = _PyObject_GC_Malloc(size);
	if (op == NULL)
		return NULL;
	assert(tp->tp_itemsize == 0);
//	if (tp->tp_flags & Py_TPFLAGS_HEAPTYPE)
//		printf("New obj type %s %d\n", tp->tp_name, Py_RefcntSnoop(tp));

	Py_TYPE(op) = tp;
	_Py_NewReference(op);
	Py_INCREF(tp);
	if (!PyType_HasFeature(tp, Py_TPFLAGS_SKIPWIPE)) {
		memset(((char *)op) + sizeof(PyObject), '\0',
			size - sizeof(PyObject));
		if (PyType_IS_GC(tp))
			gc_track(op);
	}

	return op;
}

PyObject *
_PyObject_NewVar(PyTypeObject *tp, Py_ssize_t nitems)
{
	/* XXX FIXME Some code assumes a sentinal is allocated.  Blah. */
	const size_t size = _PyObject_VAR_SIZE(tp, nitems + 1);
	PyObject *op = _PyObject_GC_Malloc(size);
	if (op == NULL)
		return NULL;
	assert(tp->tp_itemsize != 0);
//	if (tp->tp_flags & Py_TPFLAGS_HEAPTYPE)
//		printf("Newvar obj type %s %d\n", tp->tp_name, Py_RefcntSnoop(tp));

	Py_SIZE(op) = nitems;
	Py_TYPE(op) = tp;
	_Py_NewReference(op);
	Py_INCREF(tp);
	if (!PyType_HasFeature(tp, Py_TPFLAGS_SKIPWIPE)) {
		memset(((char *)op) + sizeof(PyVarObject), '\0',
			size - sizeof(PyVarObject));
		if (PyType_IS_GC(tp))
			gc_track(op);
	}

	return op;
}

/* Should only be used during tp_dealloc, or in a failed *_New.
 * Requires object have only one reference, which is consumed. */
void
_PyObject_Del(PyObject *op)
{
    PyState *pystate = PyState_Get();
    PyTypeObject *tp = Py_TYPE(op);
//    if (tp->tp_flags & Py_TPFLAGS_HEAPTYPE)
//        printf("Del obj type %s %d\n", tp->tp_name, Py_RefcntSnoop(tp));
//
    if (pystate->critical_section == NULL)
        /* Not only does this ensure the refcnt assert still works, but
         * it also prevents any debugging tools from accidentally
         * grabbing a reference to the about-to-be-deleted object. */
        Py_FatalError("PyObject_Del requires a critical section");

    /* If called during *_New it's probably still tracked (unless
     * SKIPWIPE is used) */
    if (is_tracked(op))
        gc_untrack(op);

    assert(Py_RefcntSnoop(op) == 1);
    op->ob_refowner = Py_REFOWNER_DELETED;
    op->ob_refcnt = Py_REFCNT_DELETED;
    /* XXX Rename this.  It should call a private _Free function */
    _PyObject_GC_Del(op);
    Py_DECREF(tp);
}

/* Revives an object who's tp_dealloc was called, but wasn't actually
 * deleted.  Causes the GC to track it again.
 *
 * Objects with a weakref field cannot be revived. */
void
_PyObject_Revive(PyObject *op)
{
    if (PyType_SUPPORTS_WEAKREFS(Py_TYPE(op)))
        Py_FatalError("Cannot revive objects that support weakrefs");

    if (PyType_IS_GC(Py_TYPE(op)))
        gc_track(op);
}

/* Tracks op, "completing" the allocation process.  This is only useful
 * for base classes that would rather wipe themselves lazily.
 *
 * _PyObject_Resize cannot be used until the object has been completed. */
void
_PyObject_Complete(PyObject *op)
{
	if (PyType_HasFeature(Py_TYPE(op), Py_TPFLAGS_SKIPWIPE) &&
			PyType_IS_GC(Py_TYPE(op)))
		gc_track(op);
}

PyObject *
_PyObject_Resize(PyObject *op, Py_ssize_t nitems)
{
	size_t oldsize = _PyObject_VAR_SIZE(Py_TYPE(op), Py_SIZE(op));
	/* XXX FIXME Some code assumes a sentinal is allocated.  Blah. */
	size_t newsize = _PyObject_VAR_SIZE(Py_TYPE(op), nitems + 1);

	_Py_DEC_REFTOTAL();
	if (PyType_IS_GC(Py_TYPE(op)))
		gc_untrack(op);

	/* XXX FIXME we're leaving op with its reference forgotten and untracked */
	_Py_ForgetReference(op);
	/* XXX FIXME _PyObject_GC_Resize should use newsize, not nitems */
	op = (PyObject *)_PyObject_GC_Resize((PyVarObject *)op, nitems);
	if (op == NULL)
		return NULL;
	_Py_NewReference(op);

	/* Zero out items added by growing */
	if (newsize > oldsize)
		memset(((char *)op) + oldsize, 0, newsize - oldsize);
	if (PyType_IS_GC(Py_TYPE(op)))
		gc_track(op);
	return op;
}
