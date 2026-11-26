#include "blox7/graphics/etc2_decoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace blox7::graphics {
namespace {

constexpr std::array<int, 8> kDistance = {3, 6, 11, 16, 23, 32, 41, 64};

constexpr int kEtc1Modifiers[8][4] = {
    {2, 8, -2, -8},     {5, 17, -5, -17},   {9, 29, -9, -29},
    {13, 42, -13, -42}, {18, 60, -18, -60}, {24, 80, -24, -80},
    {33, 106, -33, -106}, {47, 183, -47, -183},
};

constexpr int kEacModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14},  {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12},  {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},  {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},   {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},   {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},    {-3, -5, -7, -9, 2, 4, 6, 8},
};

std::uint8_t Clamp255(int value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

int Expand4(int value) { return (value << 4) | value; }
int Expand5(int value) { return (value << 3) | (value >> 2); }
int Expand6(int value) { return (value << 2) | (value >> 4); }
int Expand7(int value) { return (value << 1) | (value >> 6); }

constexpr std::uint8_t kRowMajor[16] = {0, 4, 8,  12, 1, 5, 9,  13,
                                        2, 6, 10, 14, 3, 7, 11, 15};

std::uint32_t Opaque(int r, int g, int b) {
  return static_cast<std::uint32_t>(Clamp255(r)) |
         static_cast<std::uint32_t>(Clamp255(g)) << 8 |
         static_cast<std::uint32_t>(Clamp255(b)) << 16 | 0xFF000000U;
}

void DecodeRgbBlock(const std::uint8_t* b, bool punchthrough,
                    std::uint32_t* out) {
  static constexpr int kDelta[8] = {0, 1, 2, 3, -4, -3, -2, -1};
  const bool diff_or_opaque = (b[3] & 2) != 0;
  const bool opaque = !punchthrough || diff_or_opaque;
  const std::uint32_t msb = (static_cast<std::uint32_t>(b[4]) << 8) | b[5];
  const std::uint32_t lsb = (static_cast<std::uint32_t>(b[6]) << 8) | b[7];

  const int r_sum = (b[0] >> 3) + kDelta[b[0] & 7];
  const int g_sum = (b[1] >> 3) + kDelta[b[1] & 7];
  const int b_sum = (b[2] >> 3) + kDelta[b[2] & 7];
  const bool individual = !punchthrough && !diff_or_opaque;

  if (!individual && (r_sum < 0 || r_sum > 31 || g_sum < 0 || g_sum > 31)) {
    std::uint32_t paint[4];
    if (r_sum < 0 || r_sum > 31) {
      const int c1[3] = {Expand4((((b[0] >> 3) & 3) << 2) | (b[0] & 3)),
                         Expand4(b[1] >> 4), Expand4(b[1] & 15)};
      const int c2[3] = {Expand4(b[2] >> 4), Expand4(b[2] & 15),
                         Expand4(b[3] >> 4)};
      const int d = kDistance[(((b[3] >> 2) & 3) << 1) | (b[3] & 1)];
      paint[0] = Opaque(c1[0], c1[1], c1[2]);
      paint[1] = Opaque(c2[0] + d, c2[1] + d, c2[2] + d);
      paint[2] = Opaque(c2[0], c2[1], c2[2]);
      paint[3] = Opaque(c2[0] - d, c2[1] - d, c2[2] - d);
    } else {
      const int c1[3] = {
          Expand4((b[0] >> 3) & 15),
          Expand4(((b[0] & 7) << 1) | ((b[1] >> 4) & 1)),
          Expand4((b[1] & 8) | ((b[1] & 3) << 1) | (b[2] >> 7))};
      const int c2[3] = {Expand4((b[2] >> 3) & 15),
                         Expand4(((b[2] & 7) << 1) | (b[3] >> 7)),
                         Expand4((b[3] >> 3) & 15)};
      const int v1 = (c1[0] << 16) | (c1[1] << 8) | c1[2];
      const int v2 = (c2[0] << 16) | (c2[1] << 8) | c2[2];
      const int d =
          kDistance[(b[3] & 4) | ((b[3] & 1) << 1) | (v1 >= v2 ? 1 : 0)];
      paint[0] = Opaque(c1[0] + d, c1[1] + d, c1[2] + d);
      paint[1] = Opaque(c1[0] - d, c1[1] - d, c1[2] - d);
      paint[2] = Opaque(c2[0] + d, c2[1] + d, c2[2] + d);
      paint[3] = Opaque(c2[0] - d, c2[1] - d, c2[2] - d);
    }
    if (!opaque) {
      paint[2] = 0;
    }
    for (int texel = 0; texel < 16; ++texel) {
      out[kRowMajor[texel]] =
          paint[(((msb >> texel) & 1) << 1) | ((lsb >> texel) & 1)];
    }
    return;
  }

  if (!individual && (b_sum < 0 || b_sum > 31)) {
    const int ro = Expand6((b[0] >> 1) & 63);
    const int go = Expand7(((b[0] & 1) << 6) | ((b[1] >> 1) & 63));
    const int bo = Expand6(((b[1] & 1) << 5) | (b[2] & 0x18) |
                           ((b[2] & 3) << 1) | (b[3] >> 7));
    const int rh = Expand6(((b[3] & 0x7c) >> 1) | (b[3] & 1));
    const int gh = Expand7(b[4] >> 1);
    const int bh = Expand6(((b[4] & 1) << 5) | (b[5] >> 3));
    const int rv = Expand6(((b[5] & 7) << 3) | (b[6] >> 5));
    const int gv = Expand7(((b[6] & 31) << 2) | (b[7] >> 6));
    const int bv = Expand6(b[7] & 63);
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        out[y * 4 + x] =
            Opaque((x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2,
                   (x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2,
                   (x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2);
      }
    }
    return;
  }

  const int tables[2] = {b[3] >> 5, (b[3] >> 2) & 7};
  std::uint32_t colors[2][4];
  for (int sub = 0; sub < 2; ++sub) {
    int base[3];
    for (int c = 0; c < 3; ++c) {
      if (individual) {
        base[c] = Expand4(sub == 0 ? b[c] >> 4 : b[c] & 15);
      } else {
        base[c] =
            Expand5((b[c] >> 3) + (sub == 0 ? 0 : kDelta[b[c] & 7]));
      }
    }
    for (int index = 0; index < 4; ++index) {
      const int modifier =
          (!opaque && index == 0) ? 0 : kEtc1Modifiers[tables[sub]][index];
      colors[sub][index] =
          Opaque(base[0] + modifier, base[1] + modifier, base[2] + modifier);
    }
    if (!opaque) {
      colors[sub][2] = 0;
    }
  }
  const int sub_shift = (b[3] & 1) != 0 ? 1 : 3;
  for (int texel = 0; texel < 16; ++texel) {
    out[kRowMajor[texel]] =
        colors[(texel >> sub_shift) & 1]
              [(((msb >> texel) & 1) << 1) | ((lsb >> texel) & 1)];
  }
}

std::uint64_t EacIndices(const std::uint8_t* b) {
  std::uint64_t bits = 0;
  for (int i = 2; i < 8; ++i) {
    bits = (bits << 8) | b[i];
  }
  return bits;
}

void DecodeAlphaBlock(const std::uint8_t* b, std::uint32_t* out) {
  const int* modifiers = kEacModifiers[b[1] & 15];
  const int multiplier = b[1] >> 4;
  std::uint32_t alpha[8];
  for (int index = 0; index < 8; ++index) {
    alpha[index] = static_cast<std::uint32_t>(
                       Clamp255(b[0] + modifiers[index] * multiplier))
                   << 24;
  }
  const std::uint64_t bits = EacIndices(b);
  for (int texel = 0; texel < 16; ++texel) {
    std::uint32_t& pixel = out[kRowMajor[texel]];
    pixel = (pixel & 0x00FFFFFFU) | alpha[(bits >> (45 - texel * 3)) & 7];
  }
}

void DecodeR11Block(const std::uint8_t* b, bool is_signed,
                    std::uint16_t* out) {
  const int* modifiers = kEacModifiers[b[1] & 15];
  const int multiplier = b[1] >> 4;
  std::uint16_t values[8];
  for (int index = 0; index < 8; ++index) {
    const int scaled = multiplier != 0 ? modifiers[index] * multiplier * 8
                                       : modifiers[index];
    if (!is_signed) {
      const int value = std::clamp(b[0] * 8 + 4 + scaled, 0, 2047);
      values[index] = static_cast<std::uint16_t>((value << 5) | (value >> 6));
      continue;
    }
    const int base =
        std::max(-127, static_cast<int>(static_cast<std::int8_t>(b[0])));
    const int value = std::clamp(base * 8 + scaled, -1023, 1023);
    const int magnitude = value < 0 ? -value : value;
    const int expanded = (magnitude << 5) | (magnitude >> 5);
    values[index] = static_cast<std::uint16_t>(
        static_cast<std::int16_t>(value < 0 ? -expanded : expanded));
  }
  const std::uint64_t bits = EacIndices(b);
  for (int texel = 0; texel < 16; ++texel) {
    out[kRowMajor[texel]] = values[(bits >> (45 - texel * 3)) & 7];
  }
}

template <typename Texel>
void StoreBlock(const Texel* texels, std::uint8_t* target, std::size_t stride,
                std::uint32_t columns, std::uint32_t rows) {
  if (columns == 4) {
    for (std::uint32_t y = 0; y < rows; ++y) {
      std::memcpy(target + y * stride, texels + y * 4, 4 * sizeof(Texel));
    }
    return;
  }
  for (std::uint32_t y = 0; y < rows; ++y) {
    std::memcpy(target + y * stride, texels + y * 4, columns * sizeof(Texel));
  }
}

}  // namespace

std::size_t EtcBlockBytes(EtcFormat format) {
  switch (format) {
    case EtcFormat::kEtc2Rgba8:
    case EtcFormat::kEacRg11:
    case EtcFormat::kEacRg11Signed:
      return 16;
    default:
      return 8;
  }
}

std::size_t EtcDecodedTexelBytes(EtcFormat format) {
  switch (format) {
    case EtcFormat::kEacR11:
    case EtcFormat::kEacR11Signed:
      return 2;
    default:
      return 4;
  }
}

namespace {

constexpr std::uint64_t kBlocksPerBand = 1024;
constexpr std::uint64_t kParallelMinimumBlocks = 4096;

bool ImageFits(EtcFormat format, const std::uint8_t* source,
               std::size_t source_bytes, std::uint32_t width,
               std::uint32_t height, const std::uint8_t* destination,
               std::size_t destination_bytes) {
  if (source == nullptr || destination == nullptr || width == 0 ||
      height == 0) {
    return false;
  }
  const std::uint64_t blocks_wide = (static_cast<std::uint64_t>(width) + 3) / 4;
  const std::uint64_t blocks_high =
      (static_cast<std::uint64_t>(height) + 3) / 4;
  return blocks_wide * blocks_high * EtcBlockBytes(format) <= source_bytes &&
         static_cast<std::uint64_t>(width) * height *
                 EtcDecodedTexelBytes(format) <=
             destination_bytes;
}

}  // namespace

bool DecodeEtcImage(EtcFormat format, const std::uint8_t* source,
                    std::size_t source_bytes, std::uint32_t width,
                    std::uint32_t height, std::uint8_t* destination,
                    std::size_t destination_bytes) {
  return DecodeEtcImageBlockRows(format, source, source_bytes, width, height, 0,
                                 (height + 3) / 4, destination,
                                 destination_bytes);
}

namespace {

struct DecodeBand {
  EtcDecodeJob* job;
  std::uint32_t first_block_row;
  std::uint32_t block_row_count;
};

struct DecodeBandQueue {
  std::vector<DecodeBand> bands;
  std::atomic<std::size_t> next{0};
  std::mutex mutex;
  std::condition_variable completed;
  unsigned workers = 0;

  void Run() {
    for (;;) {
      const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
      if (index >= bands.size()) {
        return;
      }
      const DecodeBand& band = bands[index];
      const EtcDecodeJob& job = *band.job;
      if (job.rows != nullptr) {
        job.rows(job, band.first_block_row, band.block_row_count);
        continue;
      }
      DecodeEtcImageBlockRows(job.format, job.source, job.source_bytes,
                              job.width, job.height, band.first_block_row,
                              band.block_row_count, job.destination,
                              job.destination_bytes);
    }
  }
};

class DecodeWorkerPool {
 public:
  DecodeWorkerPool() {
    const unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
    const unsigned limit = std::min(7U, hardware - 1);
    while (worker_count_ < limit) {
      if (pthread_create(&workers_[worker_count_], nullptr, Entry, this) != 0) {
        break;
      }
      ++worker_count_;
    }
  }

  ~DecodeWorkerPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
    for (unsigned index = 0; index < worker_count_; ++index) {
      pthread_join(workers_[index], nullptr);
    }
  }

  void Run(DecodeBandQueue* queue, unsigned workers) {
    const unsigned helpers = std::min(workers, worker_count_);
    if (helpers == 0) {
      queue->Run();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue->workers = helpers;
      for (unsigned index = 0; index < helpers; ++index) {
        pending_.push_back(queue);
      }
    }
    ready_.notify_all();
    queue->Run();
    // don't wait on helpers that never showed up, a busy vm can leave them asleep for ms
    unsigned withdrawn = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (*it == queue) {
          it = pending_.erase(it);
          ++withdrawn;
        } else {
          ++it;
        }
      }
    }
    std::unique_lock<std::mutex> lock(queue->mutex);
    queue->workers -= withdrawn;
    queue->completed.wait(lock, [queue] { return queue->workers == 0; });
  }

 private:
  static void* Entry(void* context) {
    static_cast<DecodeWorkerPool*>(context)->RunWorker();
    return nullptr;
  }

  void RunWorker() {
    for (;;) {
      DecodeBandQueue* queue;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
        if (pending_.empty()) {
          return;
        }
        queue = pending_.front();
        pending_.pop_front();
      }
      queue->Run();
      std::lock_guard<std::mutex> lock(queue->mutex);
      if (--queue->workers == 0) {
        queue->completed.notify_one();
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<DecodeBandQueue*> pending_;
  std::array<pthread_t, 7> workers_{};
  unsigned worker_count_ = 0;
  bool stopping_ = false;
};

}  // namespace

void DecodeEtcJobs(EtcDecodeJob* jobs, std::size_t count,
                   unsigned worker_count) {
  if (jobs == nullptr) {
    return;
  }
  DecodeBandQueue queue;
  std::vector<DecodeBand>& bands = queue.bands;
  std::uint64_t total_blocks = 0;
  for (std::size_t index = 0; index < count; ++index) {
    EtcDecodeJob& job = jobs[index];
    job.ok =
        job.rows != nullptr
            ? job.source != nullptr && job.destination != nullptr &&
                  job.width != 0 && job.height != 0
            : ImageFits(job.format, job.source, job.source_bytes, job.width,
                        job.height, job.destination, job.destination_bytes);
    if (!job.ok) {
      continue;
    }
    const std::uint32_t blocks_wide =
        (static_cast<std::uint64_t>(job.width) + 3) / 4;
    const std::uint32_t blocks_high =
        (static_cast<std::uint64_t>(job.height) + 3) / 4;
    const std::uint32_t rows_per_band = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, kBlocksPerBand / blocks_wide));
    for (std::uint32_t row = 0; row < blocks_high; row += rows_per_band) {
      bands.push_back(
          {&job, row, std::min(rows_per_band, blocks_high - row)});
    }
    total_blocks += static_cast<std::uint64_t>(blocks_wide) * blocks_high;
  }

  const std::size_t threads =
      total_blocks < kParallelMinimumBlocks
          ? 1
          : std::min<std::size_t>(std::max(worker_count, 1U), bands.size());
  if (threads <= 1) {
    queue.Run();
  } else {
    static DecodeWorkerPool pool;
    pool.Run(&queue, static_cast<unsigned>(threads - 1));
  }
}

bool DecodeEtcImageBlockRows(EtcFormat format, const std::uint8_t* source,
                             std::size_t source_bytes, std::uint32_t width,
                             std::uint32_t height,
                             std::uint32_t first_block_row,
                             std::uint32_t block_row_count,
                             std::uint8_t* destination,
                             std::size_t destination_bytes) {
  if (!ImageFits(format, source, source_bytes, width, height, destination,
                 destination_bytes)) {
    return false;
  }
  const std::uint64_t blocks_wide = (static_cast<std::uint64_t>(width) + 3) / 4;
  const std::uint64_t blocks_high =
      (static_cast<std::uint64_t>(height) + 3) / 4;
  if (static_cast<std::uint64_t>(first_block_row) + block_row_count >
      blocks_high) {
    return false;
  }
  const std::size_t texel_bytes = EtcDecodedTexelBytes(format);
  const std::size_t block_bytes = EtcBlockBytes(format);

  const bool is_signed = format == EtcFormat::kEacR11Signed ||
                         format == EtcFormat::kEacRg11Signed;
  const std::size_t stride = static_cast<std::size_t>(width) * texel_bytes;
  const std::uint64_t end_block_row =
      static_cast<std::uint64_t>(first_block_row) + block_row_count;
  for (std::uint64_t by = first_block_row; by < end_block_row; ++by) {
    const std::uint32_t rows = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(4, height - by * 4));
    const std::uint8_t* block = source + by * blocks_wide * block_bytes;
    std::uint8_t* target = destination + by * 4 * stride;
    for (std::uint64_t bx = 0; bx < blocks_wide;
         ++bx, block += block_bytes, target += 4 * texel_bytes) {
      const std::uint32_t columns = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(4, width - bx * 4));
      std::uint32_t wide[16];
      std::uint16_t narrow[16];
      std::uint16_t second[16];
      switch (format) {
        case EtcFormat::kEtc2Rgb8:
          DecodeRgbBlock(block, false, wide);
          StoreBlock(wide, target, stride, columns, rows);
          break;
        case EtcFormat::kEtc2Rgb8A1:
          DecodeRgbBlock(block, true, wide);
          StoreBlock(wide, target, stride, columns, rows);
          break;
        case EtcFormat::kEtc2Rgba8:
          DecodeRgbBlock(block + 8, false, wide);
          DecodeAlphaBlock(block, wide);
          StoreBlock(wide, target, stride, columns, rows);
          break;
        case EtcFormat::kEacR11:
        case EtcFormat::kEacR11Signed:
          DecodeR11Block(block, is_signed, narrow);
          StoreBlock(narrow, target, stride, columns, rows);
          break;
        case EtcFormat::kEacRg11:
        case EtcFormat::kEacRg11Signed:
          DecodeR11Block(block, is_signed, narrow);
          DecodeR11Block(block + 8, is_signed, second);
          for (int texel = 0; texel < 16; ++texel) {
            wide[texel] = narrow[texel] |
                          static_cast<std::uint32_t>(second[texel]) << 16;
          }
          StoreBlock(wide, target, stride, columns, rows);
          break;
      }
    }
  }
  return true;
}

}  // namespace blox7::graphics
