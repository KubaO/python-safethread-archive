
#ifndef Py_PYTHREAD_H
#define Py_PYTHREAD_H

/* Incomplete types are intentionally used here.  Only thread_*.h has
 * the full definitions. */
typedef struct _PyThread_type_lock PyThread_type_lock;
typedef struct _PyThread_type_sem PyThread_type_sem;
typedef struct _PyThread_type_cond PyThread_type_cond;
typedef struct _PyThread_type_key PyThread_type_key;
typedef struct _PyThread_type_handle PyThread_type_handle;
typedef struct _PyThread_type_timeout PyThread_type_timeout;
typedef struct _PyThread_type_flag PyThread_type_flag;

#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(void) PyThread_init_thread(void);
PyAPI_FUNC(int) PyThread_start_new_thread(PyThread_type_handle **,
    void (*)(void *), void *);

PyAPI_FUNC(PyThread_type_handle *) PyThread_get_handle(void);
PyAPI_FUNC(void) PyThread_free_handle(PyThread_type_handle *);
PyAPI_FUNC(void) PyThread_send_signal(PyThread_type_handle *, int signum);

PyAPI_FUNC(size_t) PyThread_get_stacksize(void);
PyAPI_FUNC(int) PyThread_set_stacksize(size_t);

PyAPI_FUNC(PyThread_type_lock* ) PyThread_lock_allocate(void);
PyAPI_FUNC(void) PyThread_lock_free(PyThread_type_lock *);
PyAPI_FUNC(void) PyThread_lock_acquire(PyThread_type_lock *);
PyAPI_FUNC(int) PyThread_lock_tryacquire(PyThread_type_lock *);
PyAPI_FUNC(void) PyThread_lock_release(PyThread_type_lock *);

PyAPI_FUNC(PyThread_type_sem *) PyThread_sem_allocate(int);
PyAPI_FUNC(void) PyThread_sem_free(PyThread_type_sem *);
PyAPI_FUNC(void) PyThread_sem_acquire(PyThread_type_sem *);
PyAPI_FUNC(void) PyThread_sem_release(PyThread_type_sem *);

PyAPI_FUNC(PyThread_type_cond *) PyThread_cond_allocate(void);
PyAPI_FUNC(void) PyThread_cond_free(PyThread_type_cond *);
PyAPI_FUNC(void) PyThread_cond_wait(PyThread_type_cond *,
    PyThread_type_lock *);
PyAPI_FUNC(void) PyThread_cond_timedwait(PyThread_type_cond *,
    PyThread_type_lock *, PyThread_type_timeout *);
PyAPI_FUNC(void) PyThread_cond_wakeone(PyThread_type_cond *);
PyAPI_FUNC(void) PyThread_cond_wakeall(PyThread_type_cond *);

/* Thread Local Storage (TLS) API */
PyAPI_FUNC(PyThread_type_key *) PyThread_create_key(void);
PyAPI_FUNC(void) PyThread_delete_key(PyThread_type_key *);
PyAPI_FUNC(void) PyThread_set_key_value(PyThread_type_key *, void *);
PyAPI_FUNC(void *) PyThread_get_key_value(PyThread_type_key *);
PyAPI_FUNC(void) PyThread_delete_key_value(PyThread_type_key *);

PyAPI_FUNC(PyThread_type_timeout *) PyThread_timeout_allocate(void);
PyAPI_FUNC(void) PyThread_timeout_free(PyThread_type_timeout *);
PyAPI_FUNC(void) PyThread_timeout_set(PyThread_type_timeout *, double delay);
/* A timeout might only be marked as expired after being used.  Sleeping
 * and calling PyThread_timeout_expired may not be sufficient. */
PyAPI_FUNC(int) PyThread_timeout_expired(PyThread_type_timeout *);

/* It is assumed that PyThread_flag_set and PyThread_flag_clear are only
 * called while holding some common lock.  Only PyThread_flag_wait
 * should be called without holding said common lock.
 *
 * It is also assumed that there is only one thread calling
 * PyThread_flag_wait and it is that thread which later calls
 * PyThread_flag_clear */
PyAPI_FUNC(PyThread_type_flag *) PyThread_flag_allocate(void);
PyAPI_FUNC(void) PyThread_flag_free(PyThread_type_flag *);
PyAPI_FUNC(void) PyThread_flag_set(PyThread_type_flag *);
PyAPI_FUNC(void) PyThread_flag_clear(PyThread_type_flag *);
PyAPI_FUNC(void) PyThread_flag_wait(PyThread_type_flag *);
PyAPI_FUNC(int) PyThread_flag_timedwait(PyThread_type_flag *, double delay);

#ifdef __cplusplus
}
#endif

#endif /* !Py_PYTHREAD_H */
