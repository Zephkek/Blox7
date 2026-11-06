#ifndef BLOX7_COMPAT_BIONIC_SEMAPHORE_RUNTIME_H_
#define BLOX7_COMPAT_BIONIC_SEMAPHORE_RUNTIME_H_

#include <semaphore.h>

extern "C" {

int blox7_bionic_sem_init(sem_t* semaphore, int process_shared,
                             unsigned int value);
int blox7_bionic_sem_destroy(sem_t* semaphore);
int blox7_bionic_sem_wait(sem_t* semaphore);
int blox7_bionic_sem_post(sem_t* semaphore);

}  // extern "C"

#endif  // BLOX7_COMPAT_BIONIC_SEMAPHORE_RUNTIME_H_
