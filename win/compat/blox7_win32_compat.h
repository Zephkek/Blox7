// boringssl cosplaying as openssl 3
#pragma once

#ifdef __cplusplus
#include <sys/types.h>

#if __ANDROID_API__ < 34
extern "C" ssize_t copy_file_range(int fd_in, off64_t* off_in, int fd_out, off64_t* off_out, size_t length,
                                   unsigned int flags);
#endif

#if __has_include(<openssl/base.h>)
#define EVP_MD_get_size EVP_MD_size
#define evp_md_ctx_st env_md_ctx_st
#endif
#endif
