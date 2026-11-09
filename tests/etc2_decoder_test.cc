#include "blox7/graphics/etc2_decoder.h"

#include <gtest/gtest.h>
#include <pthread.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {
std::atomic<unsigned> thread_creations{0};
}

extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*,
                                     void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread,
                                     const pthread_attr_t* attributes,
                                     void* (*entry)(void*), void* argument) {
  thread_creations.fetch_add(1, std::memory_order_relaxed);
  return __real_pthread_create(thread, attributes, entry, argument);
}

namespace blox7::graphics {
namespace {

struct ImageFixture {
  explicit ImageFixture(EtcFormat format, std::uint32_t width = 257,
                        std::uint32_t height = 129, unsigned seed = 191)
      : source(((width + 3) / 4) * ((height + 3) / 4) * EtcBlockBytes(format)),
        expected(width * height * EtcDecodedTexelBytes(format)),
        output(expected.size() + 2, 0xa5) {
    std::uint32_t value = seed;
    for (auto& byte : source) {
      value ^= value << 13;
      value ^= value >> 17;
      value ^= value << 5;
      byte = static_cast<std::uint8_t>(value);
    }
    job = {format, source.data(),     source.size(),   width,
           height, output.data() + 1, expected.size(), false};
    EXPECT_TRUE(DecodeEtcImage(format, source.data(), source.size(), width,
                               height, expected.data(), expected.size()));
  }

  void Check() const {
    EXPECT_TRUE(job.ok);
    EXPECT_EQ(output.front(), 0xa5);
    EXPECT_EQ(output.back(), 0xa5);
    EXPECT_EQ(std::vector<std::uint8_t>(output.begin() + 1, output.end() - 1),
              expected);
  }

  std::vector<std::uint8_t> source;
  std::vector<std::uint8_t> expected;
  std::vector<std::uint8_t> output;
  EtcDecodeJob job;
};

TEST(EtcDecoderTest, ParallelBatchesMatchSerialForEveryFormat) {
  for (EtcFormat format :
       {EtcFormat::kEtc2Rgb8, EtcFormat::kEtc2Rgb8A1, EtcFormat::kEtc2Rgba8,
        EtcFormat::kEacR11, EtcFormat::kEacR11Signed, EtcFormat::kEacRg11,
        EtcFormat::kEacRg11Signed}) {
    for (unsigned workers : {0U, 1U, 2U, 8U}) {
      SCOPED_TRACE(::testing::Message()
                   << static_cast<int>(format) << " workers=" << workers);
      ImageFixture image(format);
      DecodeEtcJobs(&image.job, 1, workers);
      image.Check();
    }
  }
}

TEST(EtcDecoderTest, ReusesThreadsAcrossBatches) {
  ImageFixture image(EtcFormat::kEtc2Rgba8, 512, 512);
  DecodeEtcJobs(&image.job, 1, 8);
  const unsigned created = thread_creations.load();
  for (int repeat = 0; repeat < 12; ++repeat) {
    DecodeEtcJobs(&image.job, 1, 8);
    image.Check();
  }
  EXPECT_EQ(thread_creations.load(), created);
}

TEST(EtcDecoderTest, ConcurrentCallersKeepTheirResultsIndependent) {
  std::array<std::thread, 4> callers;
  for (unsigned index = 0; index < callers.size(); ++index) {
    callers[index] = std::thread([index] {
      ImageFixture image(EtcFormat::kEtc2Rgba8, 256, 256, index + 1);
      for (int repeat = 0; repeat < 12; ++repeat) {
        DecodeEtcJobs(&image.job, 1, 8);
        image.Check();
      }
    });
  }
  for (auto& caller : callers) caller.join();
}

TEST(EtcDecoderTest, InvalidJobsDoNotPreventValidJobsFromCompleting) {
  ImageFixture image(EtcFormat::kEtc2Rgba8);
  auto truncated = image.job;
  --truncated.source_bytes;
  auto short_output = image.job;
  --short_output.destination_bytes;
  std::array<EtcDecodeJob, 4> jobs{{{}, truncated, image.job, short_output}};
  DecodeEtcJobs(jobs.data(), jobs.size(), 8);
  EXPECT_FALSE(jobs[0].ok);
  EXPECT_FALSE(jobs[1].ok);
  EXPECT_FALSE(jobs[3].ok);
  image.job = jobs[2];
  image.Check();
  DecodeEtcJobs(nullptr, 1, 8);
  DecodeEtcJobs(jobs.data(), 0, 8);
}

std::vector<std::uint8_t> SplitMixBlocks(std::size_t bytes,
                                         std::uint64_t seed) {
  std::vector<std::uint8_t> blocks(bytes);
  std::uint64_t state = seed;
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < bytes; ++index) {
    if (index % 8 == 0) {
      state += 0x9e3779b97f4a7c15ULL;
      value = state;
      value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
      value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
      value ^= value >> 31;
    }
    blocks[index] = static_cast<std::uint8_t>(value >> (8 * (index % 8)));
  }
  return blocks;
}

TEST(EtcDecoderTest, MatchesEtcpackReferenceDecodes) {
  struct Golden {
    EtcFormat format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint64_t hash;
  };
  const Golden goldens[] = {
      {EtcFormat::kEtc2Rgb8, 512, 512, 0x1378e4e000bdff18ULL},
      {EtcFormat::kEtc2Rgb8, 61, 37, 0x0216931d4f595b92ULL},
      {EtcFormat::kEtc2Rgb8A1, 512, 512, 0x15178aa3a9e8cd07ULL},
      {EtcFormat::kEtc2Rgb8A1, 61, 37, 0x40e83884261637e7ULL},
      {EtcFormat::kEtc2Rgba8, 512, 512, 0x1cc30770d3efef92ULL},
      {EtcFormat::kEtc2Rgba8, 61, 37, 0xc4e12960a0d6f3fcULL},
      {EtcFormat::kEacR11, 512, 512, 0x813ac7dad0049a5aULL},
      {EtcFormat::kEacR11, 61, 37, 0x893ca2f91eb421e8ULL},
      {EtcFormat::kEacR11Signed, 512, 512, 0xd1cbee6b382d40beULL},
      {EtcFormat::kEacR11Signed, 61, 37, 0x1a13f60f65aa3a52ULL},
      {EtcFormat::kEacRg11, 512, 512, 0xbcd08d8ce18ff1d1ULL},
      {EtcFormat::kEacRg11, 61, 37, 0x24df216efc128c7dULL},
      {EtcFormat::kEacRg11Signed, 512, 512, 0xd974154dfb89bcddULL},
      {EtcFormat::kEacRg11Signed, 61, 37, 0x50178929c5297eb7ULL},
  };
  for (const Golden& golden : goldens) {
    SCOPED_TRACE(static_cast<int>(golden.format));
    SCOPED_TRACE(golden.width);
    const std::size_t blocks =
        ((golden.width + 3) / 4) * ((golden.height + 3) / 4);
    const std::vector<std::uint8_t> source = SplitMixBlocks(
        blocks * EtcBlockBytes(golden.format),
        0x0b1053eedULL + static_cast<std::uint64_t>(golden.format) * 1000 +
            golden.width);
    std::vector<std::uint8_t> decoded(static_cast<std::size_t>(golden.width) *
                                      golden.height *
                                      EtcDecodedTexelBytes(golden.format));
    ASSERT_TRUE(DecodeEtcImage(golden.format, source.data(), source.size(),
                               golden.width, golden.height, decoded.data(),
                               decoded.size()));
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::uint8_t byte : decoded) {
      hash ^= byte;
      hash *= 0x100000001b3ULL;
    }
    EXPECT_EQ(hash, golden.hash);
  }
}

TEST(EtcDecoderTest, SmallBatchesDoNotCreateThreads) {
  const unsigned created = thread_creations.load();
  ImageFixture image(EtcFormat::kEtc2Rgb8, 37, 33);
  DecodeEtcJobs(&image.job, 1, 8);
  image.Check();
  EXPECT_EQ(thread_creations.load(), created);
}

}
}
