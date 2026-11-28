#include "wv2unpack.h"

#include <bearssl_hash.h>

#include <Bcj2.h>
#include <LzmaDec.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <string>

#include "wv2files.h"
#include "wv2setup.h"

namespace {

constexpr SizeT kChunk = 1 << 20;

void* take(ISzAllocPtr, size_t size) { return std::malloc(size); }
void give(ISzAllocPtr, void* block) { std::free(block); }
const ISzAlloc kAlloc = {take, give};

struct Buffer {
  explicit Buffer(size_t size) : data(static_cast<Byte*>(std::malloc(size))) {}
  ~Buffer() { std::free(data); }
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Byte* data;
};

struct Unpack {
  HANDLE package = INVALID_HANDLE_VALUE;
  HANDLE out = INVALID_HANDLE_VALUE;
  std::wstring dir;
  int all = 0;
  wv2_progress progress = nullptr;
  void* context = nullptr;
  int error = WV2_OK;
  DWORD win32 = 0;
  size_t next = 0;
  ULONGLONG at = 0;
  ULONGLONG written = 0;
  ULONGLONG total = 0;
  br_sha256_context hash;
};

struct Main {
  CLzmaDec lz;
  Buffer in{kChunk};
  SizeT in_at = 0;
  SizeT in_size = 0;
  ULONGLONG read = 0;
  ULONGLONG decoded = 0;
  ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
};

bool chosen(const wv2_file& file, int all) { return all || file.fixed; }

int write_failure(DWORD error) {
  if (error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL) return WV2_ERROR_DISK_SPACE;
  if (error == ERROR_FILENAME_EXCED_RANGE) return WV2_ERROR_PATH_TOO_LONG;
  return WV2_ERROR_BLOCKED_FILE;
}

bool fail(Unpack& u, int error, DWORD win32) {
  if (u.error == WV2_OK) {
    u.error = error;
    u.win32 = win32;
  }
  return false;
}

bool fail_write(Unpack& u) {
  const DWORD error = GetLastError();
  return fail(u, write_failure(error), error);
}

bool same_digest(const Byte* digest, const char* hex) {
  static const char digits[] = "0123456789abcdef";
  for (int i = 0; i < 32; ++i)
    if (hex[2 * i] != digits[digest[i] >> 4] || hex[2 * i + 1] != digits[digest[i] & 15]) return false;
  return true;
}

bool read_at(Unpack& u, ULONGLONG offset, Byte* out, DWORD size) {
  OVERLAPPED where{};
  where.Offset = static_cast<DWORD>(offset);
  where.OffsetHigh = static_cast<DWORD>(offset >> 32);
  DWORD got = 0;
  if (!ReadFile(u.package, out, size, &got, &where)) return fail(u, WV2_ERROR_BLOCKED_FILE, GetLastError());
  return got == size || fail(u, WV2_ERROR_MISMATCH, 0);
}

bool open_file(Unpack& u, const wv2_file& file) {
  const std::wstring path = u.dir + L"\\" + file.name;
  for (size_t i = u.dir.size() + 1; i < path.size(); ++i)
    if (path[i] == L'\\' && !CreateDirectoryW(path.substr(0, i).c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
      return fail_write(u);
  u.out = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (u.out == INVALID_HANDLE_VALUE) return fail_write(u);
  br_sha256_init(&u.hash);
  return true;
}

bool write_out(Unpack& u, const Byte* data, size_t size) {
  DWORD done = 0;
  if (!WriteFile(u.out, data, static_cast<DWORD>(size), &done, nullptr) || done != size) return fail_write(u);
  br_sha256_update(&u.hash, data, size);
  u.written += size;
  return true;
}

bool close_file(Unpack& u, const wv2_file& file) {
  Byte digest[br_sha256_SIZE];
  br_sha256_out(&u.hash, digest);
  const bool flushed = FlushFileBuffers(u.out);
  const DWORD error = flushed ? 0 : GetLastError();
  CloseHandle(u.out);
  u.out = INVALID_HANDLE_VALUE;
  if (!flushed) return fail(u, write_failure(error), error);
  return same_digest(digest, file.sha256) || fail(u, WV2_ERROR_MISMATCH, 0);
}

bool route(Unpack& u, const Byte* data, size_t size) {
  while (size) {
    if (u.next == WV2_ALL_FILES) {
      u.at += size;
      return true;
    }
    const wv2_file& file = wv2_files[u.next];
    if (u.at < file.offset) {
      const size_t skip = static_cast<size_t>(std::min<ULONGLONG>(size, file.offset - u.at));
      u.at += skip;
      data += skip;
      size -= skip;
      continue;
    }
    const ULONGLONG end = static_cast<ULONGLONG>(file.offset) + file.size;
    const size_t part = static_cast<size_t>(std::min<ULONGLONG>(size, end - u.at));
    if (chosen(file, u.all)) {
      if (u.at == file.offset && !open_file(u, file)) return false;
      if (!write_out(u, data, part)) return false;
    }
    u.at += part;
    data += part;
    size -= part;
    if (u.at == end) {
      if (chosen(file, u.all) && !close_file(u, file)) return false;
      ++u.next;
    }
  }
  return true;
}

bool unpack_lzma(Unpack& u, ULONGLONG at, DWORD packed, Byte* out, SizeT size, const Byte* props) {
  Buffer in(packed);
  if (!in.data) return fail(u, WV2_ERROR_MEMORY, ERROR_NOT_ENOUGH_MEMORY);
  if (!read_at(u, at, in.data, packed)) return false;
  SizeT made = size;
  SizeT used = packed;
  ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
  const SRes result =
      LzmaDecode(out, &made, in.data, &used, props, LZMA_PROPS_SIZE, LZMA_FINISH_END, &status, &kAlloc);
  if (result == SZ_ERROR_MEM) return fail(u, WV2_ERROR_MEMORY, ERROR_NOT_ENOUGH_MEMORY);
  return (result == SZ_OK && made == size && used == packed && status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK) ||
         fail(u, WV2_ERROR_MISMATCH, 0);
}

bool refill(Unpack& u, Main& m, CBcj2Dec& bcj) {
  if (m.decoded == WV2_MAIN_BYTES) return fail(u, WV2_ERROR_MISMATCH, 0);
  if (m.lz.dicPos == m.lz.dicBufSize) m.lz.dicPos = 0;
  const SizeT start = m.lz.dicPos;
  const SizeT step = static_cast<SizeT>(
      std::min<ULONGLONG>(std::min<ULONGLONG>(kChunk, m.lz.dicBufSize - start), WV2_MAIN_BYTES - m.decoded));
  const SizeT limit = start + step;
  const ELzmaFinishMode mode = m.decoded + step == WV2_MAIN_BYTES ? LZMA_FINISH_END : LZMA_FINISH_ANY;
  while (m.lz.dicPos < limit) {
    if (m.in_at == m.in_size) {
      if (m.read == WV2_MAIN_PACKED) return fail(u, WV2_ERROR_MISMATCH, 0);
      m.in_size = static_cast<SizeT>(std::min<ULONGLONG>(kChunk, WV2_MAIN_PACKED - m.read));
      if (!read_at(u, WV2_MAIN_AT + m.read, m.in.data, static_cast<DWORD>(m.in_size))) return false;
      m.read += m.in_size;
      m.in_at = 0;
    }
    SizeT used = m.in_size - m.in_at;
    const SizeT before = m.lz.dicPos;
    const SRes result = LzmaDec_DecodeToDic(&m.lz, limit, m.in.data + m.in_at, &used, mode, &m.status);
    m.in_at += used;
    if (result != SZ_OK || (used == 0 && m.lz.dicPos == before)) return fail(u, WV2_ERROR_MISMATCH, 0);
  }
  bcj.bufs[BCJ2_STREAM_MAIN] = m.lz.dic + start;
  bcj.lims[BCJ2_STREAM_MAIN] = m.lz.dic + limit;
  m.decoded += step;
  return true;
}

bool run(Unpack& u, Main& m, const Byte* call, const Byte* jump, const Byte* rc, Byte* out) {
  CBcj2Dec bcj;
  Bcj2Dec_Init(&bcj);
  bcj.bufs[BCJ2_STREAM_MAIN] = bcj.lims[BCJ2_STREAM_MAIN] = m.lz.dic;
  bcj.bufs[BCJ2_STREAM_CALL] = call;
  bcj.lims[BCJ2_STREAM_CALL] = call + WV2_CALL_BYTES;
  bcj.bufs[BCJ2_STREAM_JUMP] = jump;
  bcj.lims[BCJ2_STREAM_JUMP] = jump + WV2_JUMP_BYTES;
  bcj.bufs[BCJ2_STREAM_RC] = rc;
  bcj.lims[BCJ2_STREAM_RC] = rc + WV2_RC_BYTES;
  ULONGLONG produced = 0;
  while (produced < WV2_ARCHIVE_BYTES) {
    bcj.dest = out;
    bcj.destLim = out + std::min<ULONGLONG>(kChunk, WV2_ARCHIVE_BYTES - produced);
    if (Bcj2Dec_Decode(&bcj) != SZ_OK) return fail(u, WV2_ERROR_MISMATCH, 0);
    const size_t made = bcj.dest - out;
    produced += made;
    if (made) {
      if (!route(u, out, made)) return false;
      if (u.progress) u.progress(u.context, u.written, u.total);
    }
    if (produced == WV2_ARCHIVE_BYTES) break;
    if (bcj.state == BCJ2_STREAM_MAIN) {
      if (!refill(u, m, bcj)) return false;
    } else if (bcj.state < BCJ2_DEC_STATE_ORIG_0 || bcj.state > BCJ2_DEC_STATE_ORIG || made == 0) {
      return fail(u, WV2_ERROR_MISMATCH, 0);
    }
  }
  bool drained = Bcj2Dec_IsMaybeFinished(&bcj);
  for (int stream = 0; stream < BCJ2_NUM_STREAMS; ++stream) drained = drained && bcj.bufs[stream] == bcj.lims[stream];
  const bool finished = m.decoded == WV2_MAIN_BYTES && m.read == WV2_MAIN_PACKED && m.in_at == m.in_size &&
                        m.status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK;
  return (drained && finished && u.next == WV2_ALL_FILES) || fail(u, WV2_ERROR_MISMATCH, 0);
}

bool decode(Unpack& u) {
  const Byte call_props[] = WV2_CALL_PROPS;
  const Byte jump_props[] = WV2_JUMP_PROPS;
  const Byte main_props[] = WV2_MAIN_PROPS;
  Buffer call(WV2_CALL_BYTES), jump(WV2_JUMP_BYTES), rc(WV2_RC_BYTES), out(kChunk);
  Main m;
  LzmaDec_CONSTRUCT(&m.lz);
  if (!call.data || !jump.data || !rc.data || !out.data || !m.in.data)
    return fail(u, WV2_ERROR_MEMORY, ERROR_NOT_ENOUGH_MEMORY);
  const SRes allocated = LzmaDec_Allocate(&m.lz, main_props, LZMA_PROPS_SIZE, &kAlloc);
  if (allocated != SZ_OK)
    return fail(u, allocated == SZ_ERROR_MEM ? WV2_ERROR_MEMORY : WV2_ERROR_INTERNAL,
                allocated == SZ_ERROR_MEM ? ERROR_NOT_ENOUGH_MEMORY : 0);
  LzmaDec_Init(&m.lz);
  const bool ok = unpack_lzma(u, WV2_CALL_AT, WV2_CALL_PACKED, call.data, WV2_CALL_BYTES, call_props) &&
                  unpack_lzma(u, WV2_JUMP_AT, WV2_JUMP_PACKED, jump.data, WV2_JUMP_BYTES, jump_props) &&
                  read_at(u, WV2_RC_AT, rc.data, WV2_RC_BYTES) && run(u, m, call.data, jump.data, rc.data, out.data);
  LzmaDec_Free(&m.lz, &kAlloc);
  return ok;
}

}  // namespace

ULONGLONG wv2_package_bytes() { return WV2_PACKAGE_BYTES; }

const char* wv2_package_sha256() { return WV2_PACKAGE_SHA256; }

ULONGLONG wv2_unpack_bytes(int all) { return all ? WV2_ALL_BYTES : WV2_FIXED_BYTES; }

size_t wv2_unpack_longest(int all) {
  size_t longest = 0;
  for (const wv2_file& file : wv2_files)
    if (chosen(file, all)) longest = std::max(longest, std::wcslen(file.name));
  return longest;
}

int wv2_unpack(const wchar_t* package, const wchar_t* out_dir, int all, wv2_progress progress, void* context,
               DWORD* win32) {
  Unpack u;
  u.dir = out_dir;
  u.all = all;
  u.progress = progress;
  u.context = context;
  u.total = wv2_unpack_bytes(all);
  u.package = CreateFileW(package, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  LARGE_INTEGER size{};
  if (u.package == INVALID_HANDLE_VALUE) {
    fail(u, WV2_ERROR_BLOCKED_FILE, GetLastError());
  } else if (!GetFileSizeEx(u.package, &size) || static_cast<ULONGLONG>(size.QuadPart) != WV2_PACKAGE_BYTES) {
    fail(u, WV2_ERROR_MISMATCH, 0);
  } else {
    decode(u);
  }
  if (u.out != INVALID_HANDLE_VALUE) CloseHandle(u.out);
  if (u.package != INVALID_HANDLE_VALUE) CloseHandle(u.package);
  if (win32 != nullptr) *win32 = u.win32;
  return u.error;
}
