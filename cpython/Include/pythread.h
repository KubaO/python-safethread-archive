
#ifndef Py_PYTHREAD_H
#define Py_PYTHREAD_H

#define NO_EXIT_PROG		/* don't define PyThread_exit_prog() */
				/* (the result is no use of signals on SGI) */

typedef void *PyThread_type_lock;
typedef void *PyThread_type_sem;
typedef void *PyThread_type_cond;
typedef void *PyThread_type_key;

#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(void) PyThread_init_thread(void);
PyAPI_FUNC(long) PyThread_start_new_thread(void (*)(void *), void *);
PyAPI_FUNC(void) PyThread_exit_thread(void);
PyAPI_FUNC(void) PyThread__PyThread_exit_thread(void);
PyAPI_FUNC(long) PyThread_get_thread_ident(void);

PyAPI_FUNC(size_t) PyThread_get_stacksize(void);
PyAPI_FUNC(int) PyThread_set_stacksize(size_t);

#ifndef NO_EXIT_PROG
PyAPI_FUNC(void) PyThread_exit_prog(int);
PyAPI_FUNC(void) PyThread__PyThread_exit_prog(int);
#endif

PyAPI_FUNC(PyThread_type_lock) PyThread_lock_allocate(void);
PyAPI_FUNC(void) PyThread_lock_free(PyThread_type_lock);
PyAPI_FUNC(void) PyThread_lock_acquire(PyThread_type_lock);
PyAPI_FUNC(int) _PyThread_lock_tryacquire(PyThread_type_lock);
PyAPI_FUNC(void) PyThread_lock_release(PyThread_type_lock);

PyAPI_FUNC(PyThread_type_sem) PyThread_sem_allocate(int);
PyAPI_FUNC(void) PyThread_sem_free(PyThread_type_sem);
PyAPI_FUNC(void) PyThread_sem_wait(PyThread_type_sem);
PyAPI_FUNC(void) PyThread_sem_post(PyThread_type_sem);

PyAPI_FUNC(PyThread_type_cond) PyThread_cond_allocate(void);
PyAPI_FUNC(void) PyThread_cond_free(PyThread_type_cond);
PyAPI_FUNC(void) PyThread_cond_wait(PyThread_type_cond, PyThread_type_lock);
PyAPI_FUNC(void) PyThread_cond_wakeone(PyThread_type_cond);
PyAPI_FUNC(void) PyThread_cond_wakeall(PyThread_type_cond);

/* Thread Local Storage (TLS) API */
PyAPI_FUNC(PyThread_type_key) PyThread_create_key(void);
PyAPI_FUNC(void) PyThread_delete_key(PyThread_type_key);
PyAPI_FUNC(void) PyThread_set_key_value(PyThread_type_key, void *);
PyAPI_FUNC(void *) PyThread_get_key_value(PyThread_type_key);
PyAPI_FUNC(void) PyThread_delete_key_value(PyThread_type_key);

#ifdef __cplusplus
}
#endif

#endif /* !Py_PYTHREAD_H */
