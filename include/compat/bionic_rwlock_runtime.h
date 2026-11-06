#ifndef BLOX7_COMPAT_BIONIC_RWLOCK_RUNTIME_H_
#define BLOX7_COMPAT_BIONIC_RWLOCK_RUNTIME_H_

#include <pthread.h>

extern "C" {

int blox7_bionic_pthread_rwlock_init(pthread_rwlock_t* rwlock,
                                        const pthread_rwlockattr_t* attr);
int blox7_bionic_pthread_rwlock_destroy(pthread_rwlock_t* rwlock);
int blox7_bionic_pthread_rwlock_rdlock(pthread_rwlock_t* rwlock);
int blox7_bionic_pthread_rwlock_wrlock(pthread_rwlock_t* rwlock);
int blox7_bionic_pthread_rwlock_unlock(pthread_rwlock_t* rwlock);

}  // extern "C"

#endif  // BLOX7_COMPAT_BIONIC_RWLOCK_RUNTIME_H_
