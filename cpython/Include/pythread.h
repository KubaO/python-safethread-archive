
#ifndef Py_PYTHREAD_H
#define Py_PYTHREAD_H

typedef void *PyThread_type_lock;
typedef void *PyThread_type_sem;
typedef void *PyThread_type_cond;
typedef void *PyThread_type_key;
typedef struct {
    /* Notes:
     *   * We wrap pthread_t in a struct to ensure it's not misused.
     *   * This is intended to be an opaque struct.
     *   * No way to compare them is currently provided.  Compare PyThreadStates instead.
     *   * No "not set" value is provided.  Put a flag beside it if you need one.
     *   * Currently, the only use is for PyThread_send_signal.
     */

    /* XXX FIXME this needs to conditionalize the definition to whatever
     * thread library is in use */
    pthread_t _value;
} PyThread_type_handle;

#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(void) PyThread_init_thread(void);
PyAPI_FUNC(int) PyThread_start_new_thread(PyThread_type_handle *, void (*)(void *), void *);

PyAPI_FUNC(PyThread_type_handle) PyThread_get_handle(void);
PyAPI_FUNC(void) PyThread_send_signal(PyThread_type_handle, int signum);

PyAPI_FUNC(size_t) PyThread_get_stacksize(void);
PyAPI_FUNC(int) PyThread_set_stacksize(size_t);

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
