#ifndef BLOX7_COMPAT_BIONIC_STDIO_RUNTIME_H_
#define BLOX7_COMPAT_BIONIC_STDIO_RUNTIME_H_

#include <sys/types.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace blox7::compat {

constexpr size_t kBionicFileSize = 152;
constexpr size_t kBionicStandardStreamCount = 3;

}  // namespace blox7::compat

extern "C" {

alignas(sizeof(void*)) extern unsigned char
    __sF[blox7::compat::kBionicStandardStreamCount]
        [blox7::compat::kBionicFileSize];

}  // extern "C"

namespace blox7::compat {

inline FILE* TranslateBionicFile(FILE* stream) noexcept {
  const uintptr_t address = reinterpret_cast<uintptr_t>(stream);
  if (__builtin_expect(address == reinterpret_cast<uintptr_t>(&__sF[0]), 0)) {
    return stdin;
  }
  if (__builtin_expect(address == reinterpret_cast<uintptr_t>(&__sF[1]), 0)) {
    return stdout;
  }
  if (__builtin_expect(address == reinterpret_cast<uintptr_t>(&__sF[2]), 0)) {
    return stderr;
  }
  return stream;
}

void* BionicFileArraySymbolAddress() noexcept;
void* BionicStdinSymbolAddress() noexcept;
void* BionicStdoutSymbolAddress() noexcept;
void* BionicStderrSymbolAddress() noexcept;

}  // namespace blox7::compat

extern "C" {

size_t blox7_fwrite(const void* buffer, size_t size, size_t count,
                       FILE* stream);
size_t blox7_fread(void* buffer, size_t size, size_t count, FILE* stream);
int blox7_fflush(FILE* stream);
int blox7_fclose(FILE* stream);
int blox7_feof(FILE* stream);
int blox7_ferror(FILE* stream);
void blox7_clearerr(FILE* stream);
int blox7_fileno(FILE* stream);
int blox7_fseek(FILE* stream, long offset, int whence);
long blox7_ftell(FILE* stream);
int blox7_fseeko(FILE* stream, off_t offset, int whence);
off_t blox7_ftello(FILE* stream);
char* blox7_fgets(char* string, int count, FILE* stream);
int blox7_fputc(int character, FILE* stream);
int blox7_fputs(const char* string, FILE* stream);
int blox7_getc(FILE* stream);
wint_t blox7_fputwc(wchar_t character, FILE* stream);
int blox7_setvbuf(FILE* stream, char* buffer, int mode, size_t size);
int blox7_ungetc(int character, FILE* stream);
int blox7_vfprintf(FILE* stream, const char* format, va_list arguments);
int blox7_fprintf(FILE* stream, const char* format, ...);
int blox7_vfscanf(FILE* stream, const char* format, va_list arguments);
int blox7_fscanf(FILE* stream, const char* format, ...);
size_t blox7___fread_chk(void* buffer, size_t buffer_size, size_t size,
                            size_t count, FILE* stream);
size_t blox7___fwrite_chk(const void* buffer, size_t buffer_size,
                             size_t size, size_t count, FILE* stream);

}  // extern "C"

#endif  // BLOX7_COMPAT_BIONIC_STDIO_RUNTIME_H_
