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
	int count; /* count of allocations or collections of younger
		      generations */
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

static int enabled = 1; /* automatic collection enabled? */

/* true if we are currently running the collector */
static int collecting = 0;

/* list of uncollectable objects */
static PyObject *garbage = NULL;

/* Python string to use if unhandled exception occurs */
static PyObject *gc_str = NULL;

/* Python string used to look for __del__ attribute. */
static PyObject *delstr = NULL;

/* set for debugging information */
#define DEBUG_STATS		(1<<0) /* print collection statistics */
#define DEBUG_COLLECTABLE	(1<<1) /* print collectable objects */
#define DEBUG_UNCOLLECTABLE	(1<<2) /* print uncollectable objects */
#define DEBUG_OBJECTS		(1<<4) /* print other objects */
#define DEBUG_SAVEALL		(1<<5) /* save all garbage in gc.garbage */
#define DEBUG_LEAK		DEBUG_COLLECTABLE | \
				DEBUG_UNCOLLECTABLE | \
				DEBUG_OBJECTS | \
				DEBUG_SAVEALL
static int debug;
static PyObject *tmod = NULL;

static PyThread_type_lock PyGC_lock;

/*--------------------------------------------------------------------------
gc_refs values.

Between collections, every gc'ed object has one of two gc_refs values:

GC_UNTRACKED
    The initial state; objects returned by PyObject_GC_Malloc are in this
    state.  The object doesn't live in any generation list, and its
    tp_traverse slot must not be called.

GC_REACHABLE
    The object lives in some generation list, and its tp_traverse is safe to
    call.  An object transitions to GC_REACHABLE when PyObject_GC_Track
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
#define GC_UNTRACKED			_PyGC_REFS_UNTRACKED
#define GC_REACHABLE			_PyGC_REFS_REACHABLE
#define GC_TENTATIVELY_UNREACHABLE	_PyGC_REFS_TENTATIVELY_UNREACHABLE

#define IS_TRACKED(o) ((AS_GC(o))->ob_refcnt_trace != GC_UNTRACKED)
#define IS_REACHABLE(o) (((PyObject *)(o))->ob_refcnt_trace == GC_REACHABLE)
#define IS_TENTATIVELY_UNREACHABLE(o) ( \
	((PyObject *)(o))->ob_refcnt_trace == GC_TENTATIVELY_UNREACHABLE)

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


/* Set all gc_refs = ob_refcnt.  After this, gc_refs is > 0 for all objects
 * in containers, and is GC_REACHABLE for all tracked gc objects not in
 * containers.
 */
static void
update_refs(PyGC_Head *containers)
{
	PyGC_Head *gc = containers->ob_next;
	for (; gc != containers; gc = gc->ob_next) {
		assert(gc->ob_refcnt_trace == GC_REACHABLE);
		gc->ob_refcnt_trace = Py_RefcntSnoop(FROM_GC(gc));
		/* Python's cyclic gc should never see an incoming refcount
		 * of 0:  if something decref'ed to 0, it should have been
		 * deallocated immediately at that time.
		 * Possible cause (if the assert triggers):  a tp_dealloc
		 * routine left a gc-aware object tracked during its teardown
		 * phase, and did something-- or allowed something to happen --
		 * that called back into Python.  gc can trigger then, and may
		 * see the still-tracked dying object.  Before this assert
		 * was added, such mistakes went on to allow gc to try to
		 * delete the object again.  In a debug build, that caused
		 * a mysterious segfault, when _Py_ForgetReference tried
		 * to remove the object from the doubly-linked list of all
		 * objects a second time.  In a release build, an actual
		 * double deallocation occurred, which leads to corruption
		 * of the allocator's internal bookkeeping pointers.  That's
		 * so serious that maybe this should be a release-build
		 * check instead of an assert?
		 */
		assert(gc->ob_refcnt_trace != 0);
	}
}

/* A traversal callback for subtract_refs. */
static int
visit_decref(PyObject *op, void *data)
{
        assert(op != NULL);
	if (PyObject_IS_GC(op)) {
		PyGC_Head *gc = AS_GC(op);
		/* We're only interested in gc_refs for objects in the
		 * generation being collected, which can be recognized
		 * because only they have positive gc_refs.
		 */
		assert(gc->ob_refcnt_trace != 0); /* else refcount was too small */
		if (gc->ob_refcnt_trace > 0)
			gc->ob_refcnt_trace--;
	}
	return 0;
}

/* Subtract internal references from gc_refs.  After this, gc_refs is >= 0
 * for all objects in containers, and is GC_REACHABLE for all tracked gc
 * objects not in containers.  The ones with gc_refs > 0 are directly
 * reachable from outside containers, and so can't be collected.
 */
static void
subtract_refs(PyGC_Head *containers)
{
	traverseproc traverse;
	PyGC_Head *gc = containers->ob_next;
	for (; gc != containers; gc=gc->ob_next) {
		traverse = Py_Type(FROM_GC(gc))->tp_traverse;
		(void) traverse(FROM_GC(gc),
			       (visitproc)visit_decref,
			       NULL);
	}
}

/* A traversal callback for move_unreachable. */
static int
visit_reachable(PyObject *op, PyGC_Head *reachable)
{
	if (PyObject_IS_GC(op)) {
		PyGC_Head *gc = AS_GC(op);
		const Py_ssize_t gc_refs = gc->ob_refcnt_trace;

		if (gc_refs == 0) {
			/* This is in move_unreachable's 'young' list, but
			 * the traversal hasn't yet gotten to it.  All
			 * we need to do is tell move_unreachable that it's
			 * reachable.
			 */
			gc->ob_refcnt_trace = 1;
		}
		else if (gc_refs == GC_TENTATIVELY_UNREACHABLE) {
			/* This had gc_refs = 0 when move_unreachable got
			 * to it, but turns out it's reachable after all.
			 * Move it back to move_unreachable's 'young' list,
			 * and move_unreachable will eventually get to it
			 * again.
			 */
			gc_list_move(gc, reachable);
			gc->ob_refcnt_trace = 1;
		}
		/* Else there's nothing to do.
		 * If gc_refs > 0, it must be in move_unreachable's 'young'
		 * list, and move_unreachable will eventually get to it.
		 * If gc_refs == GC_REACHABLE, it's either in some other
		 * generation so we don't care about it, or move_unreachable
		 * already dealt with it.
		 * If gc_refs == GC_UNTRACKED, it must be ignored.
		 */
		 else {
		 	assert(gc_refs > 0
		 	       || gc_refs == GC_REACHABLE
		 	       || gc_refs == GC_UNTRACKED);
		 }
	}
	return 0;
}

/* Move the unreachable objects from young to unreachable.  After this,
 * all objects in young have gc_refs = GC_REACHABLE, and all objects in
 * unreachable have gc_refs = GC_TENTATIVELY_UNREACHABLE.  All tracked
 * gc objects not in young or unreachable still have gc_refs = GC_REACHABLE.
 * All objects in young after this are directly or indirectly reachable
 * from outside the original young; and all objects in unreachable are
 * not.
 */
static void
move_unreachable(PyGC_Head *young, PyGC_Head *unreachable)
{
	PyGC_Head *gc = young->ob_next;

	/* Invariants:  all objects "to the left" of us in young have gc_refs
	 * = GC_REACHABLE, and are indeed reachable (directly or indirectly)
	 * from outside the young list as it was at entry.  All other objects
	 * from the original young "to the left" of us are in unreachable now,
	 * and have gc_refs = GC_TENTATIVELY_UNREACHABLE.  All objects to the
	 * left of us in 'young' now have been scanned, and no objects here
	 * or to the right have been scanned yet.
	 */

	while (gc != young) {
		PyGC_Head *next;

		if (gc->ob_refcnt_trace) {
                        /* gc is definitely reachable from outside the
                         * original 'young'.  Mark it as such, and traverse
                         * its pointers to find any other objects that may
                         * be directly reachable from it.  Note that the
                         * call to tp_traverse may append objects to young,
                         * so we have to wait until it returns to determine
                         * the next object to visit.
                         */
                        PyObject *op = FROM_GC(gc);
                        traverseproc traverse = Py_Type(op)->tp_traverse;
                        assert(gc->ob_refcnt_trace > 0);
                        gc->ob_refcnt_trace = GC_REACHABLE;
                        (void) traverse(op,
                                        (visitproc)visit_reachable,
                                        (void *)young);
                        next = gc->ob_next;
		}
		else {
			/* This *may* be unreachable.  To make progress,
			 * assume it is.  gc isn't directly reachable from
			 * any object we've already traversed, but may be
			 * reachable from an object we haven't gotten to yet.
			 * visit_reachable will eventually move gc back into
			 * young if that's so, and we'll see it again.
			 */
			next = gc->ob_next;
			gc_list_move(gc, unreachable);
			gc->ob_refcnt_trace = GC_TENTATIVELY_UNREACHABLE;
		}
		gc = next;
	}
}

/* Return true if object has a finalization method.
 * CAUTION:  An instance of an old-style class has to be checked for a
 *__del__ method, and earlier versions of this used to call PyObject_HasAttr,
 * which in turn could call the class's __getattr__ hook (if any).  That
 * could invoke arbitrary Python code, mutating the object graph in arbitrary
 * ways, and that was the source of some excruciatingly subtle bugs.
 */
static int
has_finalizer(PyObject *op)
{
	if (PyGen_CheckExact(op))
		return PyGen_NeedsFinalizing((PyGenObject *)op);
	else
		return 0;
}

/* Move the objects in unreachable with __del__ methods into `finalizers`.
 * Objects moved into `finalizers` have gc_refs set to GC_REACHABLE; the
 * objects remaining in unreachable are left at GC_TENTATIVELY_UNREACHABLE.
 */
static void
move_finalizers(PyGC_Head *unreachable, PyGC_Head *finalizers)
{
	PyGC_Head *gc;
	PyGC_Head *next;

	/* March over unreachable.  Move objects with finalizers into
	 * `finalizers`.
	 */
	for (gc = unreachable->ob_next; gc != unreachable; gc = next) {
		PyObject *op = FROM_GC(gc);

		assert(IS_TENTATIVELY_UNREACHABLE(op));
		next = gc->ob_next;

		if (has_finalizer(op)) {
			gc_list_move(gc, finalizers);
			gc->ob_refcnt_trace = GC_REACHABLE;
		}
	}
}

/* A traversal callback for move_finalizer_reachable. */
static int
visit_move(PyObject *op, PyGC_Head *tolist)
{
	if (PyObject_IS_GC(op)) {
		if (IS_TENTATIVELY_UNREACHABLE(op)) {
			PyGC_Head *gc = AS_GC(op);
			gc_list_move(gc, tolist);
			gc->ob_refcnt_trace = GC_REACHABLE;
		}
	}
	return 0;
}

/* Move objects that are reachable from finalizers, from the unreachable set
 * into finalizers set.
 */
static void
move_finalizer_reachable(PyGC_Head *finalizers)
{
	traverseproc traverse;
	PyGC_Head *gc = finalizers->ob_next;
	for (; gc != finalizers; gc = gc->ob_next) {
		/* Note that the finalizers list may grow during this. */
		traverse = Py_Type(FROM_GC(gc))->tp_traverse;
		(void) traverse(FROM_GC(gc),
				(visitproc)visit_move,
				(void *)finalizers);
	}
}

/* Clear all weakrefs to unreachable objects, and if such a weakref has a
 * callback, invoke it if necessary.  Note that it's possible for such
 * weakrefs to be outside the unreachable set -- indeed, those are precisely
 * the weakrefs whose callbacks must be invoked.  See gc_weakref.txt for
 * overview & some details.  Some weakrefs with callbacks may be reclaimed
 * directly by this routine; the number reclaimed is the return value.  Other
 * weakrefs with callbacks may be moved into the `old` generation.  Objects
 * moved into `old` have gc_refs set to GC_REACHABLE; the objects remaining in
 * unreachable are left at GC_TENTATIVELY_UNREACHABLE.  When this returns,
 * no object in `unreachable` is weakly referenced anymore.
 */
static int
handle_weakrefs(PyGC_Head *unreachable, PyGC_Head *old)
{
	PyGC_Head *gc;
	PyObject *op;		/* generally FROM_GC(gc) */
	PyWeakReference *wr;	/* generally a cast of op */
	PyGC_Head *next;
	int num_freed = 0;

        /* XXX FIXME clearing cyclic objects should cause them to be
         * deleted, calling Py_Dealloc, which should clear weakrefs for
         * us.  A cleared object should never live past the tracing
         * operation, so this lazy weakref handling should be sufficient. */
#if 0
	/* Clear all weakrefs to the objects in unreachable.  If such a weakref
	 * also has a callback, move it into `wrcb_to_call` if the callback
	 * needs to be invoked.  Note that we cannot invoke any callbacks until
	 * all weakrefs to unreachable objects are cleared, lest the callback
	 * resurrect an unreachable object via a still-active weakref.  We
	 * make another pass over wrcb_to_call, invoking callbacks, after this
	 * pass completes.
	 */
	for (gc = unreachable->ob_next; gc != unreachable; gc = next) {
		PyWeakReference **wrlist;

		op = FROM_GC(gc);
		assert(IS_TENTATIVELY_UNREACHABLE(op));
		next = gc->ob_next;

		if (!PyType_SUPPORTS_WEAKREFS(Py_Type(op)))
			continue;

		/* It supports weakrefs.  Does it have any? */
		wrlist = (PyWeakReference **)
			     		PyObject_GET_WEAKREFS_LISTPTR(op);

		/* `op` may have some weakrefs.  March over the list, clear
		 * all the weakrefs, and move the weakrefs with callbacks
		 * that must be called into wrcb_to_call.
		 */
		for (wr = *wrlist; wr != NULL; wr = *wrlist) {
			PyGC_Head *wrasgc;	/* AS_GC(wr) */

			/* _PyWeakref_ClearRef clears the weakref but leaves
			 * the callback pointer intact.  Obscure:  it also
			 * changes *wrlist.
			 */
			assert(wr->wr_object == op);
			_PyWeakref_ClearRef(wr);
			assert(wr->wr_object == Py_None);
		}
	}
#endif

	return num_freed;
}

static void
debug_cycle(char *msg, PyObject *op)
{
	if (debug & DEBUG_OBJECTS) {
		PySys_WriteStderr("gc: %.100s <%.100s %p>\n",
				  msg, Py_Type(op)->tp_name, op);
	}
}

/* Handle uncollectable garbage (cycles with finalizers, and stuff reachable
 * only from such cycles).
 * If DEBUG_SAVEALL, all objects in finalizers are appended to the module
 * garbage list (a Python list), else only the objects in finalizers with
 * __del__ methods are appended to garbage.  All objects in finalizers are
 * merged into the old list regardless.
 * Returns 0 if all OK, <0 on error (out of memory to grow the garbage list).
 * The finalizers list is made empty on a successful return.
 */
static int
handle_finalizers(PyGC_Head *finalizers, PyGC_Head *old)
{
	PyGC_Head *gc = finalizers->ob_next;

	if (garbage == NULL) {
		garbage = PyList_New(0);
		if (garbage == NULL)
			Py_FatalError("gc couldn't create gc.garbage list");
	}
	for (; gc != finalizers; gc = gc->ob_next) {
		PyObject *op = FROM_GC(gc);

		if ((debug & DEBUG_SAVEALL) || has_finalizer(op)) {
			if (PyList_Append(garbage, op) < 0)
				return -1;
		}
	}

	gc_list_merge(finalizers, old);
	return 0;
}

/* Break reference cycles by clearing the containers involved.	This is
 * tricky business as the lists can be changing and we don't know which
 * objects may be freed.  It is possible I screwed something up here.
 */
static void
delete_garbage(PyGC_Head *collectable, PyGC_Head *old)
{
	inquiry clear;

	while (!gc_list_is_empty(collectable)) {
		PyGC_Head *gc = collectable->ob_next;
		PyObject *op = FROM_GC(gc);

		assert(IS_TENTATIVELY_UNREACHABLE(op));
		if (debug & DEBUG_SAVEALL) {
			PyList_Append(garbage, op);
		}
		else {
			if ((clear = Py_Type(op)->tp_clear) != NULL) {
				Py_INCREF(op);
				clear(op);
				Py_DECREF(op);
			}
		}
		if (collectable->ob_next == gc) {
			/* object is still alive, move it, it may die later */
			gc_list_move(gc, old);
			gc->ob_refcnt_trace = GC_REACHABLE;
		}
	}
}

/* This is the main function.  Read this to understand how the
 * collection process works. */
static Py_ssize_t
collect(int generation)
{
	int i;
	Py_ssize_t m = 0; /* # objects collected */
	Py_ssize_t n = 0; /* # unreachable objects that couldn't be collected */
	PyGC_Head *young; /* the generation we are examining */
	PyGC_Head *old; /* next older generation */
	PyGC_Head unreachable; /* non-problematic unreachable trash */
	PyGC_Head finalizers;  /* objects with, & reachable from, __del__ */
	PyGC_Head *gc;
	double t1 = 0.0;

	/* XXX the lists can now include objects that say GC_UNTRACKED.
	 * This means those objects are deallocated but in the cache.
	 * We need to start skipping them. */
	return 0; /* XXX FIXME HACK */
	if (delstr == NULL) {
		delstr = PyUnicode_InternFromString("__del__");
		if (delstr == NULL)
			Py_FatalError("gc couldn't allocate \"__del__\"");
	}

	if (debug & DEBUG_STATS) {
		if (tmod != NULL) {
			PyObject *f = PyObject_CallMethod(tmod, "time", NULL);
			if (f == NULL) {
				PyErr_Clear();
			}
			else {
				t1 = PyFloat_AsDouble(f);
				Py_DECREF(f);
			}
		}
		PySys_WriteStderr("gc: collecting generation %d...\n",
				  generation);
		PySys_WriteStderr("gc: objects in each generation:");
		for (i = 0; i < NUM_GENERATIONS; i++)
			PySys_WriteStderr(" %" PY_FORMAT_SIZE_T "d",
					  gc_list_size(GEN_HEAD(i)));
		PySys_WriteStderr("\n");
	}

	/* update collection and allocation counters */
	if (generation+1 < NUM_GENERATIONS)
		generations[generation+1].count += 1;
	for (i = 0; i <= generation; i++)
		generations[i].count = 0;

	/* merge younger generations with one we are currently collecting */
	for (i = 0; i < generation; i++) {
		gc_list_merge(GEN_HEAD(i), GEN_HEAD(generation));
	}

	/* handy references */
	young = GEN_HEAD(generation);
	if (generation < NUM_GENERATIONS-1)
		old = GEN_HEAD(generation+1);
	else
		old = young;

	/* Using ob_refcnt and gc_refs, calculate which objects in the
	 * container set are reachable from outside the set (i.e., have a
	 * refcount greater than 0 when all the references within the
	 * set are taken into account).
	 */
	update_refs(young);
	subtract_refs(young);

	/* Leave everything reachable from outside young in young, and move
	 * everything else (in young) to unreachable.
	 * NOTE:  This used to move the reachable objects into a reachable
	 * set instead.  But most things usually turn out to be reachable,
	 * so it's more efficient to move the unreachable things.
	 */
	gc_list_init(&unreachable);
	move_unreachable(young, &unreachable);

	/* Move reachable objects to next generation. */
	if (young != old)
		gc_list_merge(young, old);

	/* All objects in unreachable are trash, but objects reachable from
	 * finalizers can't safely be deleted.  Python programmers should take
	 * care not to create such things.  For Python, finalizers means
	 * instance objects with __del__ methods.  Weakrefs with callbacks
	 * can also call arbitrary Python code but they will be dealt with by
	 * handle_weakrefs().
 	 */
	gc_list_init(&finalizers);
	move_finalizers(&unreachable, &finalizers);
	/* finalizers contains the unreachable objects with a finalizer;
	 * unreachable objects reachable *from* those are also uncollectable,
	 * and we move those into the finalizers list too.
	 */
	move_finalizer_reachable(&finalizers);

	/* Collect statistics on collectable objects found and print
	 * debugging information.
	 */
	for (gc = unreachable.ob_next; gc != &unreachable;
			gc = gc->ob_next) {
		m++;
		if (debug & DEBUG_COLLECTABLE) {
			debug_cycle("collectable", FROM_GC(gc));
		}
		if (tmod != NULL && (debug & DEBUG_STATS)) {
			PyObject *f = PyObject_CallMethod(tmod, "time", NULL);
			if (f == NULL) {
				PyErr_Clear();
			}
			else {
				t1 = PyFloat_AsDouble(f)-t1;
				Py_DECREF(f);
				PySys_WriteStderr("gc: %.4fs elapsed.\n", t1);
			}
		}
	}

	/* Clear weakrefs and invoke callbacks as necessary. */
	m += handle_weakrefs(&unreachable, old);

	/* Call tp_clear on objects in the unreachable set.  This will cause
	 * the reference cycles to be broken.  It may also cause some objects
	 * in finalizers to be freed.
	 */
	delete_garbage(&unreachable, old);

	/* Collect statistics on uncollectable objects found and print
	 * debugging information. */
	for (gc = finalizers.ob_next;
	     gc != &finalizers;
	     gc = gc->ob_next) {
		n++;
		if (debug & DEBUG_UNCOLLECTABLE)
			debug_cycle("uncollectable", FROM_GC(gc));
	}
	if (debug & DEBUG_STATS) {
		if (m == 0 && n == 0)
			PySys_WriteStderr("gc: done.\n");
		else
			PySys_WriteStderr(
			    "gc: done, "
			    "%" PY_FORMAT_SIZE_T "d unreachable, "
			    "%" PY_FORMAT_SIZE_T "d uncollectable.\n",
			    n+m, n);
	}

	/* Append instances in the uncollectable set to a Python
	 * reachable list of garbage.  The programmer has to deal with
	 * this if they insist on creating this type of structure.
	 */
	(void)handle_finalizers(&finalizers, old);

	if (PyErr_Occurred()) {
		if (gc_str == NULL)
			gc_str = PyUnicode_FromString("garbage collection");
		PyErr_WriteUnraisable(gc_str);
		Py_FatalError("unexpected exception during garbage collection");
	}
	return n+m;
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
	enabled = 1;
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
	enabled = 0;
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
	return PyBool_FromLong((long)enabled);
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

	if (collecting)
		n = 0; /* already collecting, don't do anything */
	else {
		collecting = 1;
		n = collect(genarg);
		collecting = 0;
	}

	return PyInt_FromSsize_t(n);
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
"  DEBUG_OBJECTS - Print objects other than instances.\n"
"  DEBUG_SAVEALL - Save objects to gc.garbage rather than freeing them.\n"
"  DEBUG_LEAK - Debug leaking programs (everything but STATS).\n");

static PyObject *
gc_set_debug(PyObject *self, PyObject *args)
{
	if (!PyArg_ParseTuple(args, "i:set_debug", &debug))
		return NULL;

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
	return Py_BuildValue("i", debug);
}

PyDoc_STRVAR(gc_set_thresh__doc__,
"set_threshold(threshold0, [threshold1, threshold2]) -> None\n"
"\n"
"Sets the collection thresholds.  Setting threshold0 to zero disables\n"
"collection.\n");

static PyObject *
gc_set_thresh(PyObject *self, PyObject *args)
{
	int i;
	if (!PyArg_ParseTuple(args, "i|ii:set_threshold",
			      &generations[0].threshold,
			      &generations[1].threshold,
			      &generations[2].threshold))
		return NULL;
	for (i = 2; i < NUM_GENERATIONS; i++) {
 		/* generations higher than 2 get the same threshold */
		generations[i].threshold = generations[2].threshold;
	}

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
	return Py_BuildValue("(iii)",
			     generations[0].threshold,
			     generations[1].threshold,
			     generations[2].threshold);
}

PyDoc_STRVAR(gc_get_count__doc__,
"get_count() -> (count0, count1, count2)\n"
"\n"
"Return the current collection counts\n");

static PyObject *
gc_get_count(PyObject *self, PyObject *noargs)
{
	return Py_BuildValue("(iii)",
			     generations[0].count,
			     generations[1].count,
			     generations[2].count);
}

static int
referrersvisit(PyObject* obj, PyObject *objs)
{
	Py_ssize_t i;
	for (i = 0; i < PyTuple_GET_SIZE(objs); i++)
		if (PyTuple_GET_ITEM(objs, i) == obj)
			return 1;
	return 0;
}

static int
gc_referrers_for(PyObject *objs, PyGC_Head *list, PyObject *resultlist)
{
	PyGC_Head *gc;
	PyObject *obj;
	traverseproc traverse;
	for (gc = list->ob_next; gc != list; gc = gc->ob_next) {
		obj = FROM_GC(gc);
		traverse = Py_Type(obj)->tp_traverse;
		if (obj == objs || obj == resultlist)
			continue;
		if (traverse(obj, (visitproc)referrersvisit, objs)) {
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
	if (!result) return NULL;

	for (i = 0; i < NUM_GENERATIONS; i++) {
		if (!(gc_referrers_for(args, GEN_HEAD(i), result))) {
			Py_DECREF(result);
			return NULL;
		}
	}
	return result;
}

/* Append obj to list; return true if error (out of memory), false if OK. */
static int
referentsvisit(PyObject *obj, PyObject *list)
{
	return PyList_Append(list, obj) < 0;
}

PyDoc_STRVAR(gc_get_referents__doc__,
"get_referents(*objs) -> list\n\
Return the list of objects that are directly referred to by objs.");

static PyObject *
gc_get_referents(PyObject *self, PyObject *args)
{
	Py_ssize_t i;
	PyObject *result = PyList_New(0);

	if (result == NULL)
		return NULL;

	for (i = 0; i < PyTuple_GET_SIZE(args); i++) {
		traverseproc traverse;
		PyObject *obj = PyTuple_GET_ITEM(args, i);

		if (! PyObject_IS_GC(obj))
			continue;
		traverse = Py_Type(obj)->tp_traverse;
		if (! traverse)
			continue;
		if (traverse(obj, (visitproc)referentsvisit, result)) {
			Py_DECREF(result);
			return NULL;
		}
	}
	return result;
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
	{"enable",	   gc_enable,	  METH_NOARGS,  gc_enable__doc__},
	{"disable",	   gc_disable,	  METH_NOARGS,  gc_disable__doc__},
	{"isenabled",	   gc_isenabled,  METH_NOARGS,  gc_isenabled__doc__},
	{"set_debug",	   gc_set_debug,  METH_VARARGS, gc_set_debug__doc__},
	{"get_debug",	   gc_get_debug,  METH_NOARGS,  gc_get_debug__doc__},
	{"get_count",	   gc_get_count,  METH_NOARGS,  gc_get_count__doc__},
	{"set_threshold",  gc_set_thresh, METH_VARARGS, gc_set_thresh__doc__},
	{"get_threshold",  gc_get_thresh, METH_NOARGS,  gc_get_thresh__doc__},
	{"collect",	   (PyCFunction)gc_collect,
         	METH_VARARGS | METH_KEYWORDS,           gc_collect__doc__},
	{"get_objects",    gc_get_objects,METH_NOARGS,  gc_get_objects__doc__},
	{"get_referrers",  gc_get_referrers, METH_VARARGS,
		gc_get_referrers__doc__},
	{"get_referents",  gc_get_referents, METH_VARARGS,
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

	m = Py_InitModule4("gc",
			      GcMethods,
			      gc__doc__,
			      NULL,
			      PYTHON_API_VERSION);
	if (m == NULL)
		return;

	if (garbage == NULL) {
		garbage = PyList_New(0);
		if (garbage == NULL)
			return;
	}
	Py_INCREF(garbage);
	if (PyModule_AddObject(m, "garbage", garbage) < 0)
		return;

	/* Importing can't be done in collect() because collect()
	 * can be called via PyGC_Collect() in Py_Finalize().
	 * This wouldn't be a problem, except that <initialized> is
	 * reset to 0 before calling collect which trips up
	 * the import and triggers an assertion.
	 */
	if (tmod == NULL) {
		tmod = PyImport_ImportModule("time");
		if (tmod == NULL)
			PyErr_Clear();
	}

#define ADD_INT(NAME) if (PyModule_AddIntConstant(m, #NAME, NAME) < 0) return
	ADD_INT(DEBUG_STATS);
	ADD_INT(DEBUG_COLLECTABLE);
	ADD_INT(DEBUG_UNCOLLECTABLE);
	ADD_INT(DEBUG_OBJECTS);
	ADD_INT(DEBUG_SAVEALL);
	ADD_INT(DEBUG_LEAK);
#undef ADD_INT
}

/* API to invoke gc.collect() from C */
Py_ssize_t
PyGC_Collect(void)
{
	Py_ssize_t n;

	Py_FatalError("Cycle GC Disabled");

	if (collecting)
		n = 0; /* already collecting, don't do anything */
	else {
		collecting = 1;
		n = collect(NUM_GENERATIONS - 1);
		collecting = 0;
	}

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

#undef PyObject_GC_Track
#undef PyObject_GC_UnTrack
#undef PyObject_GC_Del
#undef _PyObject_GC_Malloc

void
PyObject_GC_Track(void *op)
{
	_PyObject_GC_TRACK(op);
}

void
PyObject_GC_UnTrack(void *arg)
{
	PyObject *op = arg;
	/* Obscure:  the Py_TRASHCAN mechanism requires that we be able to
	 * call PyObject_GC_UnTrack twice on an object.
	 */
	if (IS_TRACKED(op))
		_PyObject_GC_UNTRACK(op);
}

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
	op->ob_refowner = (AO_t)PyThreadState_Get();
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
_Py_Dealloc(PyObject *op)
{
    destructor dealloc = Py_Type(op)->tp_dealloc;
    assert(dealloc != NULL);

    if (PyType_SUPPORTS_WEAKREFS(Py_Type(op)) &&
            _PyObject_ClearWeakref(op)) {
        /* He's not dead, he's pining for the fjords! */
        Py_DECREF_ASYNC(op);
    } else {
        _Py_INC_TPFREES(op) _Py_COUNT_ALLOCS_COMMA	\
        (*dealloc)(op);
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
			Py_RefcntSnoop(op), Py_Type(op)->tp_name);
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
		       (t != NULL && Py_Type(op) != (PyTypeObject *) t)) {
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
_PyGC_AsyncRefcount_Flush(void)
{
	PyThreadState *tstate = PyThreadState_Get();
	int i;

	for (i = 0; i < Py_ASYNCREFCOUNT_TABLE; i++) {
		PyAsyncRefEntry *entry = &tstate->async_refcounts[i];
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
	PyThreadState *tstate = PyThreadState_Get();
	AO_t oldmode;

	//assert(monitorspace != NULL);
	//printf("Promoting %p\n", op);

	oldmode = AO_load_acquire(&op->ob_refowner);
	if (oldmode == Py_REFOWNER_STATICINIT)
		AO_compare_and_swap_full(&op->ob_refowner,
			Py_REFOWNER_STATICINIT, (AO_t)tstate);
	else if (oldmode == Py_REFOWNER_ASYNC) {
		/* Do nothing */
	} else {
		/* XXX FIXME this should only be a partial suspend.  We
		 * musn't allow the tracing GC to collect the
		 * PyThreadState we're about to use.  Some sort of usage
		 * count? */
		PyThreadState *owner = (PyThreadState *)oldmode;

		AO_fetch_and_add1_full(&owner->inspect_count);
		PyState_Suspend();
		PyThread_lock_acquire(owner->inspect_queue_lock);
		AO_store_full(&owner->inspect_flag, 1);
		PyThread_lock_acquire(owner->inspect_lock);
		AO_store_full(&owner->inspect_flag, 0);
		PyThread_lock_release(owner->inspect_queue_lock);

		/* Another thread may already have altered the object's
		 * refowner field, so we do another comparison. */
		AO_compare_and_swap_full(&op->ob_refowner, oldmode,
			Py_REFOWNER_ASYNC);

		PyThread_lock_release(owner->inspect_lock);
		PyState_Resume();
		AO_fetch_and_sub1_full(&owner->inspect_count);
	}
}

static PyAsyncRefEntry *
_Py_GetAsyncRefEntry(PyThreadState *tstate, PyObject *op)
{
	/* XXX this probably needs to be heavily optimized */
	PyAsyncRefEntry *entry;
	AO_t index = (AO_t)op;

	index ^= (index >> 3) ^ (index >> 7) ^ (index >> 17);
	index &= Py_ASYNCREFCOUNT_TABLE - 1;

	entry = &tstate->async_refcounts[index];

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
_Py_INCREF(PyObject *op, register PyThreadState *tstate)
{
	assert(tstate != NULL);
	assert(!tstate->suspended);

//	if (PyType_Check(op) && ((PyTypeObject *)op)->tp_flags & Py_TPFLAGS_HEAPTYPE)
//		printf("Heap type incref %s %d\n",
//			((PyTypeObject *)op)->tp_name, Py_RefcntSnoop(op));

	_Py_INC_REFTOTAL();

	/* Blah, this should be done at compile time, or maybe in configure. */
	assert(sizeof(AO_t) == sizeof(Py_ssize_t));

	while (1) {

		void *owner = (void *)AO_load_acquire(&op->ob_refowner);
		if (_Py_EXPECT(owner == Py_REFOWNER_ASYNC)) {
			/* This should use a tstate hash table */
			PyAsyncRefEntry *entry = _Py_GetAsyncRefEntry(tstate, op);
			entry->diff++;
			if (entry->diff == 0)
				entry->obj = NULL;
			else
				entry->obj = op;
			return;
		} else if (_Py_EXPECT(owner == tstate)) {
			op->ob_refcnt++;
			return;
		} else {
			_PyGC_RefMode_Promote(op);
			continue;
		}
	}
}

void
_Py_DECREF(PyObject *op, register PyThreadState *tstate)
{
	assert(tstate != NULL);
	assert(!tstate->suspended);

//	if (PyType_Check(op) && ((PyTypeObject *)op)->tp_flags & Py_TPFLAGS_HEAPTYPE)
//		printf("Heap type decref %s %d\n",
//			((PyTypeObject *)op)->tp_name, Py_RefcntSnoop(op));

	_Py_DEC_REFTOTAL();

	/* Blah, this should be done at compile time, or maybe in configure. */
	assert(sizeof(AO_t) == sizeof(Py_ssize_t));

	while (1) {
		void *owner = (void *)AO_load_acquire(&op->ob_refowner);
		if (_Py_EXPECT(owner == Py_REFOWNER_ASYNC)) {
			PyAsyncRefEntry *entry = _Py_GetAsyncRefEntry(tstate, op);
			entry->diff--;
			if (entry->diff == 0)
				entry->obj = NULL;
			else
				entry->obj = op;
			return;
		} else if (_Py_EXPECT(owner == tstate)) {
			if (op->ob_refcnt > 1)
				op->ob_refcnt--;
			else
				_Py_Dealloc(op);
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
_Py_DECREF_ASYNC(PyObject *op, register PyThreadState *tstate)
{
	assert(tstate != NULL);
	assert(!tstate->suspended);

//	if (PyType_Check(op) && ((PyTypeObject *)op)->tp_flags & Py_TPFLAGS_HEAPTYPE)
//		printf("Heap type decref %s %d\n",
//			((PyTypeObject *)op)->tp_name, Py_RefcntSnoop(op));

	_Py_DEC_REFTOTAL();

	/* Blah, this should be done at compile time, or maybe in configure. */
	assert(sizeof(AO_t) == sizeof(Py_ssize_t));

	while (1) {
		void *owner = (void *)AO_load_acquire(&op->ob_refowner);
		if (_Py_EXPECT(owner == Py_REFOWNER_ASYNC)) {
			PyAsyncRefEntry *entry = _Py_GetAsyncRefEntry(tstate, op);
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
	PyThreadState *tstate = PyThreadState_Get();
	void *owner = (void *)AO_load_acquire(&op->ob_refowner);

	if (owner == tstate)
		return op->ob_refcnt;
	else
		return 1000000;  /* Arbitrary large value */
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
	//Py_ssize_t size_class = find_size_class(sizeof(PyGC_Head) + basicsize);
	Py_ssize_t size_class = find_size_class(basicsize);

	if (size_class <= 0) {
		PyThreadState *tstate = PyThreadState_Get();
		Py_ssize_t i;

		for (i = 0; i < PYGC_CACHE_COUNT; i++) {
			if (tstate->gc_object_cache[-size_class][i] != NULL) {
				g = tstate->gc_object_cache[-size_class][i];
				tstate->gc_object_cache[-size_class][i] = NULL;
				g->ob_sizeclass = size_class;
				assert(g->ob_refcnt_trace == GC_UNTRACKED);
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
		g->ob_refcnt_trace = GC_UNTRACKED;

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
	/* XXX FIXME Some code assumes a sentinal is allocated.  Blah. */
	const size_t basicsize = _PyObject_VAR_SIZE(Py_Type(op), nitems + 1);
	PyObject *g = (PyObject *)op;
	//Py_ssize_t size_class = find_size_class(sizeof(PyGC_Head) + basicsize);
	Py_ssize_t size_class = find_size_class(basicsize);

	if (IS_TRACKED((PyObject *)op))
		Py_FatalError("_PyObject_GC_Resize called for tracked object");

	if (size_class == g->ob_sizeclass) {
		//printf("Resize avoided\n");
		Py_Size(op) = nitems;
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
	Py_Size(op) = nitems;
	return op;
}

static void
_PyObject_GC_Del(void *arg)
{
	PyGC_Head *g = AS_GC(arg);
	Py_ssize_t size_class = g->ob_sizeclass;

	assert(g == arg); /* WTF? */
	if (IS_TRACKED(g))
		_PyObject_GC_UNTRACK(g);

	if (size_class <= 0) {
		PyThreadState *tstate = PyThreadState_Get();
		Py_ssize_t i;

		for (i = 0; i < PYGC_CACHE_COUNT; i++) {
			if (tstate->gc_object_cache[-size_class][i] == NULL) {
				//printf("Filling cache\n");
				tstate->gc_object_cache[-size_class][i] = g;
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
	PyThreadState *tstate = PyThreadState_Get();
	Py_ssize_t i, j;

	PyThread_lock_acquire(PyGC_lock);
	PyGC_lock_count();

	for (i = 0; i < PYGC_CACHE_SIZECLASSES; i++) {
		for (j = 0; j < PYGC_CACHE_COUNT; j++) {
			PyGC_Head *g = tstate->gc_object_cache[i][j];
			tstate->gc_object_cache[i][j] = NULL;

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

	Py_Type(op) = tp;
	_Py_NewReference(op);
	Py_INCREF(tp);
	if (!PyType_HasFeature(tp, Py_TPFLAGS_SKIPWIPE)) {
		memset(((char *)op) + sizeof(PyObject), '\0',
			size - sizeof(PyObject));
		if (PyType_IS_GC(tp))
			_PyObject_GC_TRACK(op);
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

	Py_Size(op) = nitems;
	Py_Type(op) = tp;
	_Py_NewReference(op);
	Py_INCREF(tp);
	if (!PyType_HasFeature(tp, Py_TPFLAGS_SKIPWIPE)) {
		memset(((char *)op) + sizeof(PyVarObject), '\0',
			size - sizeof(PyVarObject));
		if (PyType_IS_GC(tp))
			_PyObject_GC_TRACK(op);
	}

	return op;
}

void
_PyObject_Del(PyObject *op)
{
	/* XXX Rename this.  It should call a private _Free function */
	PyTypeObject *tp = Py_Type(op);
//	if (tp->tp_flags & Py_TPFLAGS_HEAPTYPE)
//		printf("Del obj type %s %d\n", tp->tp_name, Py_RefcntSnoop(tp));
	assert(Py_RefcntSnoop(op) == 1);
	op->ob_refowner = Py_REFOWNER_DELETED;
	op->ob_refcnt = Py_REFCNT_DELETED;
	_PyObject_GC_Del(op);
	Py_DECREF(tp);
}

/* Tracks op, "completing" the allocation process. */
void
_PyObject_Complete(PyObject *op)
{
	if (PyType_HasFeature(Py_Type(op), Py_TPFLAGS_SKIPWIPE) &&
			PyType_IS_GC(Py_Type(op)))
		_PyObject_GC_TRACK(op);
}

PyObject *
_PyObject_Resize(PyObject *op, Py_ssize_t nitems)
{
	size_t oldsize = _PyObject_VAR_SIZE(Py_Type(op), Py_Size(op));
	/* XXX FIXME Some code assumes a sentinal is allocated.  Blah. */
	size_t newsize = _PyObject_VAR_SIZE(Py_Type(op), nitems + 1);

	_Py_DEC_REFTOTAL();
	if (PyType_IS_GC(Py_Type(op)))
		_PyObject_GC_UNTRACK(op);

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
	if (PyType_IS_GC(Py_Type(op)))
		_PyObject_GC_TRACK(op);
	return op;
}
