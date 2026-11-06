#ifndef BLOX7_COMPAT_BIONIC_SOCKET_RUNTIME_H_
#define BLOX7_COMPAT_BIONIC_SOCKET_RUNTIME_H_

#include <sys/socket.h>

extern "C" {

int blox7_bionic_setsockopt(int socket, int level, int option_name,
                               const void *option_value,
                               socklen_t option_length);

ssize_t blox7_bionic_sendmsg(int socket, const struct msghdr *message,
                                int flags);

}  // extern "C"

#endif  // BLOX7_COMPAT_BIONIC_SOCKET_RUNTIME_H_
