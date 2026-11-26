#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "blox7/graphics/etc2_decoder.h"

namespace {

using blox7::graphics::DecodeEtcImage;
using blox7::graphics::DecodeEtcImageBlockRows;
using blox7::graphics::DecodeEtcJobs;
using blox7::graphics::EtcBlockBytes;
using blox7::graphics::EtcDecodedTexelBytes;
using blox7::graphics::EtcDecodeJob;
using blox7::graphics::EtcFormat;

struct Expected {
  EtcFormat format;
  const char* name;
  std::uint64_t blocks;
  std::uint64_t sizes;
};

constexpr Expected kExpected[] = {
    {EtcFormat::kEtc2Rgb8, "rgb8", 0x8FAA21D897D3E1B5ull,
     0x458DED45297DB9ADull},
    {EtcFormat::kEtc2Rgb8A1, "rgb8a1", 0x9BCCC03A08E91B71ull,
     0x2471B534A1F40B92ull},
    {EtcFormat::kEtc2Rgba8, "rgba8", 0x366CAD1CEF22127Cull,
     0xDE2AFF2611B91E45ull},
    {EtcFormat::kEacR11, "r11", 0x6B70AC301189B781ull,
     0x2F6CDAB5A793330Full},
    {EtcFormat::kEacR11Signed, "r11s", 0xFE9D75ED2D981671ull,
     0x14D21DFEB0B89DD9ull},
    {EtcFormat::kEacRg11, "rg11", 0xA2F6C72576E0B776ull,
     0x3B4703E2777E5008ull},
    {EtcFormat::kEacRg11Signed, "rg11s", 0xF6A42124324B64D2ull,
     0xB5224B7F4E6FF8A0ull},
};

std::uint64_t g_state;

std::uint64_t Next() {
  g_state ^= g_state << 7;
  g_state ^= g_state >> 9;
  return g_state * 0x2545F4914F6CDD1Dull;
}

void Fill(std::vector<std::uint8_t>* bytes, std::size_t block) {
  for (std::size_t at = 0; at < bytes->size(); ++at) {
    (*bytes)[at] = static_cast<std::uint8_t>(Next() >> 32);
  }
  for (std::size_t at = 0; at + block <= bytes->size(); at += block) {
    const std::size_t index = at / block;
    std::uint8_t* color = bytes->data() + at + (block == 16 ? 8 : 0);
    if (index % 3 == 1) {
      color[3] |= 2;
    } else if (index % 3 == 2) {
      color[3] &= static_cast<std::uint8_t>(~2);
    }
    if (index % 101 == 0) {
      std::memset(bytes->data() + at, index % 2 != 0 ? 0xFF : 0x00, block);
    }
  }
}

std::uint64_t Hash(std::uint64_t hash, const std::vector<std::uint8_t>& bytes,
                   std::size_t size) {
  for (std::size_t at = 0; at < size; ++at) {
    hash = (hash ^ bytes[at]) * 0x100000001B3ull;
  }
  return hash;
}

double Seconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

int main() {
  int failed = 0;
  for (const Expected& expected : kExpected) {
    const EtcFormat format = expected.format;
    const std::size_t block = EtcBlockBytes(format);
    const std::size_t texel = EtcDecodedTexelBytes(format);
    g_state = 0x9E3779B97F4A7C15ull + static_cast<std::uint64_t>(format);
    bool ok = true;

    const std::uint32_t side = 512;
    std::vector<std::uint8_t> source(side / 4 * (side / 4) * block);
    std::vector<std::uint8_t> whole(side * side * texel);
    std::vector<std::uint8_t> threaded(whole.size());
    std::uint64_t blocks = 0xCBF29CE484222325ull;
    double best = 1e9;
    for (int round = 0; round < 8; ++round) {
      Fill(&source, block);
      std::memset(whole.data(), 0xA5, whole.size());
      const double start = Seconds();
      ok = DecodeEtcImage(format, source.data(), source.size(), side, side,
                          whole.data(), whole.size()) &&
           ok;
      best = std::min(best, Seconds() - start);
      blocks = Hash(blocks, whole, whole.size());
      std::memset(threaded.data(), 0x5A, threaded.size());
      EtcDecodeJob job;
      job.format = format;
      job.source = source.data();
      job.source_bytes = source.size();
      job.width = side;
      job.height = side;
      job.destination = threaded.data();
      job.destination_bytes = threaded.size();
      DecodeEtcJobs(&job, 1, 4);
      ok = ok && job.ok && threaded == whole;
    }

    std::uint64_t sizes = 0xCBF29CE484222325ull;
    for (std::uint32_t width = 1; width <= 21; ++width) {
      for (std::uint32_t height = 1; height <= 21; ++height) {
        const std::uint32_t rows = (height + 3) / 4;
        const std::size_t bytes = width * height * texel;
        std::vector<std::uint8_t> blocks_in((width + 3) / 4 * rows * block);
        std::vector<std::uint8_t> image(bytes + 8, 0xC3);
        std::vector<std::uint8_t> banded(bytes + 8, 0xC3);
        Fill(&blocks_in, block);
        ok = DecodeEtcImage(format, blocks_in.data(), blocks_in.size(), width,
                            height, image.data(), bytes) &&
             ok;
        for (std::uint32_t row = 0; row < rows; ++row) {
          ok = DecodeEtcImageBlockRows(format, blocks_in.data(),
                                       blocks_in.size(), width, height, row, 1,
                                       banded.data(), bytes) &&
               ok;
        }
        ok = ok && banded == image;
        for (std::size_t at = bytes; at < image.size(); ++at) {
          ok = ok && image[at] == 0xC3;
        }
        ok = ok && !DecodeEtcImage(format, blocks_in.data(),
                                   blocks_in.size() - 1, width, height,
                                   banded.data(), bytes);
        ok = ok && !DecodeEtcImage(format, blocks_in.data(), blocks_in.size(),
                                   width, height, banded.data(), bytes - 1);
        ok = ok && !DecodeEtcImageBlockRows(format, blocks_in.data(),
                                            blocks_in.size(), width, height,
                                            rows, 1, banded.data(), bytes);
        sizes = Hash(sizes, image, bytes);
      }
    }

    const bool same = blocks == expected.blocks && sizes == expected.sizes;
    std::printf("%-6s %s  %.2f ns per texel  0x%016llXull, 0x%016llXull\n",
                expected.name, ok && same ? "ok" : "FAILED",
                best * 1e9 / (side * side),
                static_cast<unsigned long long>(blocks),
                static_cast<unsigned long long>(sizes));
    if (!ok || !same) {
      ++failed;
    }
  }
  return failed != 0 ? 1 : 0;
}
