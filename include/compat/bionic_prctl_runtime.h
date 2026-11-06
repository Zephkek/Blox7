#ifndef BLOX7_COMPAT_BIONIC_PRCTL_RUNTIME_H_
#define BLOX7_COMPAT_BIONIC_PRCTL_RUNTIME_H_

extern "C" {

int blox7_bionic_prctl(int option, unsigned long argument2,
                          unsigned long argument3, unsigned long argument4,
                          unsigned long argument5);

}  // extern "C"

#endif  // BLOX7_COMPAT_BIONIC_PRCTL_RUNTIME_H_
