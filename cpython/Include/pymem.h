/* The PyMem_ family:  low-level memory allocation interfaces.
   See objimpl.h for the PyObject_ memory family.
*/

#ifndef Py_PYMEM_H
#define Py_PYMEM_H

#include "pyport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BEWARE:

   Each interface exports both functions and macros.  Extension modules should
   use the functions, to ensure binary compatibility across Python versions.
   Because the Python implementation is free to change internal details, and
   the macros may (or may not) expose details for speed, if you do use the
   macros you must recompile your extensions with each Python release.

   Never mix calls to PyMem_ with calls to the platform malloc/realloc/
   calloc/free.  For example, on Windows different DLLs may end up using
   different heaps, and if you use PyMem_Malloc you'll get the memory from the
   heap used by the Python DLL; it could be a disaster if you free()'ed that
   directly in your own extension.  Using PyMem_Free instead ensures Python
   can return the memory to the proper heap.  As another example, in
   PYMALLOC_DEBUG mode, Python wraps all calls to all PyMem_ and PyObject_
   memory functions in special debugging wrappers that add additional
   debugging info to dynamic memory blocks.  The system routines have no idea
   what to do with that stuff, and the Python wrappers have no idea what to do
   with raw blocks obtained directly by the system routines then.

   The GIL must be held when using these APIs.
*/

/*
 * Raw memory interface
 * ====================
 */

/* Functions

   Functions supplying platform-independent semantics for malloc/realloc/
   free.  These functions make sure that allocating 0 bytes returns a distinct
   non-NULL pointer (whenever possible -- if we're flat out of memory, NULL
   may be returned), even if the platform malloc and realloc don't.
   Returned pointers must be checked for NULL explicitly.  No action is
   performed on failure (no exception is set, no warning is printed, etc).
*/

#if 1
PyAPI_FUNC(void) Py_FatalError(const char *message);

/* XXX Must match up with obmalloc.c's size_classes */
#define PYMALLOC_CACHE_SIZECLASSES 13

#define PYMALLOC_CACHE_COUNT 32


PyAPI_FUNC(void *) pymemcache_malloc(size_t);
PyAPI_FUNC(void *) pymemcache_realloc(void *, size_t);
PyAPI_FUNC(void) pymemcache_free(void *);


/* pymemwrap is just a temporary bodge until the different names are
 * properly unified. */
PyAPI_FUNC(void *) _pymemwrap_malloc(const char *, const char *, size_t);
PyAPI_FUNC(void *) _pymemwrap_realloc(const char *, const char *, void *, size_t);
PyAPI_FUNC(void) _pymemwrap_free(const char *, const char *, void *);

#define PyMEMWRAP_MALLOC(name, group) \
static inline void * \
name(size_t size) \
{ \
	return _pymemwrap_malloc(#name, #group, size); \
}

#define PyMEMWRAP_REALLOC(name, group) \
static inline void * \
name(void *oldmem, size_t size) \
{ \
	return _pymemwrap_realloc(#name, #group, oldmem, size); \
}

#define PyMEMWRAP_FREE(name, group) \
static inline void \
name(void *mem) \
{ \
	_pymemwrap_free(#name, #group, mem); \
}

PyMEMWRAP_MALLOC(PyMem_Malloc, PyMem_Camel)
PyMEMWRAP_REALLOC(PyMem_Realloc, PyMem_Camel)
PyMEMWRAP_FREE(PyMem_Free, PyMem_Camel)

PyMEMWRAP_MALLOC(PyMem_MALLOC, PyMem_UPPER)
PyMEMWRAP_REALLOC(PyMem_REALLOC, PyMem_UPPER)
PyMEMWRAP_FREE(PyMem_FREE, PyMem_UPPER)

PyMEMWRAP_MALLOC(PyObject_Malloc, PyObject_Camel)
PyMEMWRAP_REALLOC(PyObject_Realloc, PyObject_Camel)
PyMEMWRAP_FREE(PyObject_Free, PyObject_Camel)

PyMEMWRAP_MALLOC(PyObject_MALLOC, PyObject_UPPER)
PyMEMWRAP_REALLOC(PyObject_REALLOC, PyObject_UPPER)
PyMEMWRAP_FREE(PyObject_FREE, PyObject_UPPER)

/* PyMem_Del is only used by multibytecodec.c */
PyMEMWRAP_FREE(PyMem_Del, PyMem_CamelDel)
PyMEMWRAP_FREE(PyMem_DEL, PyMem_UPPERDEL)

//PyMEMWRAP_FREE(hidden_PyObject_Del, PyObject_CamelDel)
//PyAPI_FUNC(void) PyObject_Del(void *);
//PyMEMWRAP_FREE(PyObject_DEL, PyObject_UPPERDEL)

#else
PyAPI_FUNC(void *) PyMem_Malloc(size_t);
PyAPI_FUNC(void *) PyMem_Realloc(void *, size_t);
PyAPI_FUNC(void) PyMem_Free(void *);

/* Starting from Python 1.6, the wrappers Py_{Malloc,Realloc,Free} are
   no longer supported. They used to call PyErr_NoMemory() on failure. */

/* Macros. */
#ifdef PYMALLOC_DEBUG
/* Redirect all memory operations to Python's debugging allocator. */
#define PyMem_MALLOC		PyObject_MALLOC
#define PyMem_REALLOC		PyObject_REALLOC
#define PyMem_FREE		PyObject_FREE

#else	/* ! PYMALLOC_DEBUG */

/* PyMem_MALLOC(0) means malloc(1). Some systems would return NULL
   for malloc(0), which would be treated as an error. Some platforms
   would return a pointer with no memory behind it, which would break
   pymalloc. To solve these problems, allocate an extra byte. */
#define PyMem_MALLOC(n)         malloc((n) ? (n) : 1)
#define PyMem_REALLOC(p, n)     realloc((p), (n) ? (n) : 1)
#define PyMem_FREE		free

#endif	/* PYMALLOC_DEBUG */
#endif

/*
 * Type-oriented memory interface
 * ==============================
 *
 * These are carried along for historical reasons.  There's rarely a good
 * reason to use them anymore (you can just as easily do the multiply and
 * cast yourself).
 */

#define PyMem_New(type, n) \
	( (type *) PyMem_Malloc((n) * sizeof(type)) )
#define PyMem_NEW(type, n) \
	( (type *) PyMem_MALLOC((n) * sizeof(type)) )

#define PyMem_Resize(p, type, n) \
	( (p) = (type *) PyMem_Realloc((p), (n) * sizeof(type)) )
#define PyMem_RESIZE(p, type, n) \
	( (p) = (type *) PyMem_REALLOC((p), (n) * sizeof(type)) )

/* PyMem{Del,DEL} are left over from ancient days, and shouldn't be used
 * anymore.  They're just confusing aliases for PyMem_{Free,FREE} now.
 */
//#define PyMem_Del		PyMem_Free
//#define PyMem_DEL		PyMem_FREE

#ifdef __cplusplus
}
#endif

#endif /* !Py_PYMEM_H */
