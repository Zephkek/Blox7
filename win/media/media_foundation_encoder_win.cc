#include <media/NdkMediaCodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "media/avc_sps.h"
#include "media/video_encoder.h"

extern "C" void* __w32_pe_open(const char* utf8_path);
extern "C" void* __w32_pe_symbol(void* module, const char* name);

namespace blox7::media {
namespace {

#define BLOX7_MS __attribute__((ms_abi))

using Result = std::int32_t;

struct Guid {
  std::uint32_t a;
  std::uint16_t b, c;
  std::uint8_t d[8];
};

struct TypeInfo {
  Guid major;
  Guid subtype;
};

struct OutputStreamInfo {
  std::uint32_t flags;
  std::uint32_t size;
  std::uint32_t alignment;
};

struct OutputBuffer {
  std::uint32_t stream;
  void* sample;
  std::uint32_t status;
  void* events;
};

struct Variant {
  std::uint16_t type;
  std::uint16_t reserved[3];
  union {
    std::uint32_t uint32;
    std::int16_t boolean;
    std::uint64_t storage[2];
  } value;
};

static_assert(sizeof(Guid) == 16 && sizeof(TypeInfo) == 32);
static_assert(sizeof(OutputStreamInfo) == 12);
static_assert(sizeof(OutputBuffer) == 32 && offsetof(OutputBuffer, sample) == 8 &&
              offsetof(OutputBuffer, status) == 16 &&
              offsetof(OutputBuffer, events) == 24);
static_assert(sizeof(Variant) == 24 && offsetof(Variant, value) == 8);

constexpr Guid kVideoEncoderCategory = {0xF79EAC7D, 0xE545, 0x4387, {0xBD, 0xEE, 0xD6, 0x47, 0xD7, 0xBD, 0xE4, 0x2A}};
constexpr Guid kMediaTypeVideo = {0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr Guid kFormatNv12 = {0x3231564E, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr Guid kFormatH264 = {0x34363248, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr Guid kTransformIid = {0xBF94C121, 0x5B05, 0x4E6F, {0x80, 0x00, 0xBA, 0x59, 0x89, 0x61, 0x41, 0x4D}};
constexpr Guid kEventGeneratorIid = {0x2CD0BD52, 0xBCD5, 0x4B89, {0xB6, 0x2C, 0xEA, 0xDC, 0x0C, 0x03, 0x1E, 0x7D}};
constexpr Guid kShutdownIid = {0x97EC2EA4, 0x0E42, 0x4937, {0x97, 0xAC, 0x9D, 0x6D, 0x32, 0x88, 0x24, 0xE1}};
constexpr Guid kCodecApiIid = {0x901DB4C7, 0x31CE, 0x41A2, {0x85, 0xDC, 0x8F, 0xA0, 0xBF, 0x41, 0xB8, 0xDA}};
constexpr Guid kTransformAsync = {0xF81A699A, 0x649A, 0x497D, {0x8C, 0x73, 0x29, 0xF8, 0xFE, 0xD6, 0xAD, 0x7A}};
constexpr Guid kTransformAsyncUnlock = {0xE5666D6B, 0x3422, 0x4EB6, {0xA4, 0x21, 0xDA, 0x7D, 0xB1, 0xF8, 0xE2, 0x07}};
constexpr Guid kLowLatency = {0x9C27891A, 0xED7A, 0x40E1, {0x88, 0xE8, 0xB2, 0x27, 0x27, 0xA0, 0x24, 0xEE}};
constexpr Guid kFriendlyName = {0x314FFBAE, 0x5B41, 0x4C95, {0x9C, 0x19, 0x4E, 0x7D, 0x58, 0x6F, 0xAC, 0xE3}};
constexpr Guid kMajorType = {0x48EBA18E, 0xF8C9, 0x4687, {0xBF, 0x11, 0x0A, 0x74, 0xC9, 0xF9, 0x6A, 0x8F}};
constexpr Guid kSubtype = {0xF7E34C9A, 0x42E8, 0x4714, {0xB7, 0x4B, 0xCB, 0x29, 0xD7, 0x2C, 0x35, 0xE5}};
constexpr Guid kFrameSize = {0x1652C33D, 0xD6B2, 0x4012, {0xB8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xA3, 0x7D}};
constexpr Guid kFrameRate = {0xC459A2E8, 0x3D2C, 0x4E44, {0xB1, 0x32, 0xFE, 0xE5, 0x15, 0x6C, 0x7B, 0xB0}};
constexpr Guid kPixelAspectRatio = {0xC6376A1E, 0x8D0A, 0x4027, {0xBE, 0x45, 0x6D, 0x9A, 0x0A, 0xD3, 0x9B, 0xB6}};
constexpr Guid kInterlaceMode = {0xE2724BB8, 0xE676, 0x4806, {0xB4, 0xB2, 0xA8, 0xD6, 0xEF, 0xB4, 0x4C, 0xCD}};
constexpr Guid kAverageBitrate = {0x20332624, 0xFB0D, 0x4D9E, {0xBD, 0x0D, 0xCB, 0xF6, 0x78, 0x6C, 0x10, 0x2E}};
constexpr Guid kMpeg2Profile = {0xAD76A80B, 0x2D5C, 0x4E0B, {0xB3, 0x75, 0x64, 0xE5, 0x20, 0x13, 0x70, 0x36}};
constexpr Guid kVideoPrimaries = {0xDBFBE4D7, 0x0740, 0x4EE0, {0x81, 0x92, 0x85, 0x0A, 0xB0, 0xE2, 0x19, 0x35}};
constexpr Guid kTransferFunction = {0x5FB0FCE9, 0xBE5C, 0x4935, {0xA8, 0x11, 0xEC, 0x83, 0x8F, 0x8E, 0xED, 0x93}};
constexpr Guid kYuvMatrix = {0x3E23D450, 0x2C75, 0x4D25, {0xA0, 0x0E, 0xB9, 0x16, 0x70, 0xD1, 0x23, 0x27}};
constexpr Guid kNominalRange = {0xC21B8EE5, 0xB956, 0x4071, {0x8D, 0xAF, 0x32, 0x5E, 0xDF, 0x5C, 0xAB, 0x11}};
constexpr Guid kRateControlMode = {0x1C0608E9, 0x370C, 0x4710, {0x8A, 0x58, 0xCB, 0x61, 0x81, 0xC4, 0x24, 0x23}};
constexpr Guid kMeanBitrate = {0xF7222374, 0x2144, 0x4815, {0xB5, 0x50, 0xA3, 0x7F, 0x8E, 0x12, 0xEE, 0x52}};
constexpr Guid kGopSize = {0x95F31B26, 0x95A4, 0x41AA, {0x93, 0x03, 0x24, 0x6A, 0x7F, 0xC6, 0xEE, 0xF1}};
constexpr Guid kBPictureCount = {0x8D390AAC, 0xDC5C, 0x4200, {0xB5, 0x7F, 0x81, 0x4D, 0x04, 0xBA, 0xBA, 0xB2}};
constexpr Guid kLowLatencyMode = {0x9C27891A, 0xED7A, 0x40E1, {0x88, 0xE8, 0xB2, 0x27, 0x27, 0xA0, 0x24, 0xEE}};
constexpr Guid kForceKeyFrame = {0x398C1B98, 0x8353, 0x475A, {0x9E, 0xF2, 0x8F, 0x26, 0x5D, 0x26, 0x03, 0x45}};
constexpr Guid kCabacEnable = {0xEE6CAD62, 0xD305, 0x4248, {0xA5, 0x0E, 0xE1, 0xB2, 0x55, 0xF7, 0xCA, 0xF8}};

constexpr Result kNoEventsAvailable = static_cast<Result>(0xC00D3E80);
constexpr Result kNeedMoreInput = static_cast<Result>(0xC00D6D72);
constexpr Result kStreamChange = static_cast<Result>(0xC00D6D61);
constexpr Result kNotAccepting = static_cast<Result>(0xC00D36B5);
constexpr std::uint32_t kMfVersion = 0x20070;
constexpr std::uint32_t kStartupLite = 0x1;
constexpr std::uint32_t kEnumHardware = 0x4;
constexpr std::uint32_t kEnumSortAndFilter = 0x40;
constexpr std::uint32_t kMultithreaded = 0x0;
constexpr std::uint32_t kEventError = 1;
constexpr std::uint32_t kEventNeedInput = 601;
constexpr std::uint32_t kEventHaveOutput = 602;
constexpr std::uint32_t kEventDrainComplete = 603;
constexpr std::uint32_t kEventNoWait = 0x1;
constexpr std::uint32_t kMessageDrain = 0x1;
constexpr std::uint32_t kMessageBeginStreaming = 0x10000000;
constexpr std::uint32_t kMessageEndOfStream = 0x10000002;
constexpr std::uint32_t kMessageStartOfStream = 0x10000003;
constexpr std::uint32_t kProvidesSamples = 0x100;
constexpr std::uint32_t kCanProvideSamples = 0x200;
constexpr std::uint32_t kProgressive = 2;
constexpr std::uint32_t kProfileHigh = 100;
constexpr std::uint32_t kConstantBitrate = 0;
constexpr std::uint32_t kPrimariesBt709 = 2;
constexpr std::uint32_t kTransfer709 = 5;
constexpr std::uint32_t kMatrixBt709 = 1;
constexpr std::uint32_t kRange16To235 = 2;
constexpr std::uint16_t kVariantUint32 = 19;
constexpr std::uint16_t kVariantBool = 11;
constexpr std::int16_t kVariantTrue = -1;
constexpr std::size_t kPooledSamples = 8;

enum Slot : int {
  kQueryInterface = 0,
  kAddRef = 1,
  kRelease = 2,
  kGetUint32 = 7,
  kGetAllocatedString = 13,
  kSetUint32 = 21,
  kSetUint64 = 22,
  kSetGuid = 24,
  kActivateObject = 33,
  kShutdownObject = 34,
  kGetStreamIds = 5,
  kGetOutputStreamInfo = 7,
  kGetAttributes = 8,
  kGetOutputAvailableType = 14,
  kSetInputType = 15,
  kSetOutputType = 16,
  kProcessMessage = 23,
  kProcessInput = 24,
  kProcessOutput = 25,
  kGetEvent = 3,
  kEventGetType = 33,
  kEventGetStatus = 35,
  kGetSampleTime = 35,
  kSetSampleTime = 36,
  kSetSampleDuration = 38,
  kConvertToContiguousBuffer = 41,
  kAddBuffer = 42,
  kBufferLock = 3,
  kBufferUnlock = 4,
  kBufferSetCurrentLength = 6,
  kCodecSetValue = 9,
  kShutdown = 3,
};

template <typename R, typename... Args>
R Call(void* object, Slot slot, Args... args) {
  using Method = R(BLOX7_MS*)(void*, Args...);
  return reinterpret_cast<Method>((*static_cast<void***>(object))[slot])(
      object, args...);
}

bool Failed(Result result) { return result < 0; }

std::uint32_t Release(void* object) {
  return Call<std::uint32_t>(object, kRelease);
}

bool Idle(void* object, std::uint32_t references) {
  Call<std::uint32_t>(object, kAddRef);
  return Release(object) == references;
}

class Com {
 public:
  Com() = default;
  explicit Com(void* object) : object_(object) {}
  ~Com() { reset(); }
  Com(Com&& other) noexcept : object_(std::exchange(other.object_, nullptr)) {}
  Com& operator=(Com&& other) noexcept {
    if (this != &other) {
      reset();
      object_ = std::exchange(other.object_, nullptr);
    }
    return *this;
  }
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;

  void reset() {
    if (object_ != nullptr) {
      Release(std::exchange(object_, nullptr));
    }
  }
  void** out() {
    reset();
    return &object_;
  }
  void* get() const { return object_; }
  explicit operator bool() const { return object_ != nullptr; }

 private:
  void* object_ = nullptr;
};

Result Query(void* object, const Guid& iid, Com* out) {
  return Call<Result>(object, kQueryInterface, &iid, out->out());
}

Result SetUint32(void* attributes, const Guid& key, std::uint32_t value) {
  return Call<Result>(attributes, kSetUint32, &key, value);
}

Result SetUint64(void* attributes, const Guid& key, std::uint64_t value) {
  return Call<Result>(attributes, kSetUint64, &key, value);
}

Result SetGuid(void* attributes, const Guid& key, const Guid& value) {
  return Call<Result>(attributes, kSetGuid, &key, &value);
}

std::int64_t MonotonicUs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

std::uint64_t Pack(std::uint32_t high, std::uint32_t low) {
  return (static_cast<std::uint64_t>(high) << 32) | low;
}

struct MediaFoundation {
  Result(BLOX7_MS* co_initialize)(void*, std::uint32_t) = nullptr;
  void(BLOX7_MS* co_uninitialize)() = nullptr;
  void(BLOX7_MS* co_task_mem_free)(void*) = nullptr;
  Result(BLOX7_MS* startup)(std::uint32_t, std::uint32_t) = nullptr;
  Result(BLOX7_MS* shutdown)() = nullptr;
  Result(BLOX7_MS* enumerate)(Guid, std::uint32_t, const TypeInfo*,
                              const TypeInfo*, void***, std::uint32_t*) =
      nullptr;
  Result(BLOX7_MS* create_media_type)(void**) = nullptr;
  Result(BLOX7_MS* create_sample)(void**) = nullptr;
  Result(BLOX7_MS* create_memory_buffer)(std::uint32_t, void**) = nullptr;
};

template <typename Fn>
bool Bind(void* module, const char* name, Fn* slot) {
  *slot = module != nullptr ? reinterpret_cast<Fn>(__w32_pe_symbol(module, name))
                            : nullptr;
  return *slot != nullptr;
}

const MediaFoundation* Api() {
  static const std::unique_ptr<MediaFoundation> api = [] {
    auto loaded = std::make_unique<MediaFoundation>();
    void* ole32 = __w32_pe_open("ole32.dll");
    void* mfplat = __w32_pe_open("mfplat.dll");
    const bool ok =
        Bind(ole32, "CoInitializeEx", &loaded->co_initialize) &&
        Bind(ole32, "CoUninitialize", &loaded->co_uninitialize) &&
        Bind(ole32, "CoTaskMemFree", &loaded->co_task_mem_free) &&
        Bind(mfplat, "MFStartup", &loaded->startup) &&
        Bind(mfplat, "MFShutdown", &loaded->shutdown) &&
        Bind(mfplat, "MFTEnumEx", &loaded->enumerate) &&
        Bind(mfplat, "MFCreateMediaType", &loaded->create_media_type) &&
        Bind(mfplat, "MFCreateSample", &loaded->create_sample) &&
        Bind(mfplat, "MFCreateMemoryBuffer", &loaded->create_memory_buffer);
    return ok ? std::move(loaded) : nullptr;
  }();
  return api.get();
}

class Apartment {
 public:
  explicit Apartment(const MediaFoundation* api)
      : api_(api),
        entered_(!Failed(api->co_initialize(nullptr, kMultithreaded))) {}
  ~Apartment() {
    if (entered_) {
      api_->co_uninitialize();
    }
  }
  Apartment(const Apartment&) = delete;
  Apartment& operator=(const Apartment&) = delete;

 private:
  const MediaFoundation* const api_;
  const bool entered_;
};

void EnterApartment(const MediaFoundation* api) {
  thread_local const Apartment apartment(api);
}

std::size_t FindStartCode(const std::uint8_t* data, std::size_t size,
                          std::size_t from) {
  while (from + 3 <= size) {
    const void* one = std::memchr(data + from + 2, 1, size - from - 2);
    if (one == nullptr) {
      return size;
    }
    const auto at =
        static_cast<std::size_t>(static_cast<const std::uint8_t*>(one) - data);
    if (data[at - 1] == 0 && data[at - 2] == 0) {
      return at - 2;
    }
    from = at - 1;
  }
  return size;
}

void AppendNal(std::vector<std::uint8_t>* out, const std::uint8_t* nal,
               std::size_t size) {
  static constexpr std::uint8_t kStartCode[] = {0, 0, 0, 1};
  out->insert(out->end(), kStartCode, kStartCode + sizeof kStartCode);
  out->insert(out->end(), nal, nal + size);
}

std::string FriendlyName(const MediaFoundation* api, void* activate) {
  char16_t* wide = nullptr;
  std::uint32_t length = 0;
  std::string name;
  if (!Failed(Call<Result>(activate, kGetAllocatedString, &kFriendlyName,
                           &wide, &length)) &&
      wide != nullptr) {
    for (std::uint32_t index = 0; index < length; ++index) {
      std::uint32_t code = wide[index];
      if (code >= 0xD800 && code < 0xDC00 && index + 1 < length &&
          wide[index + 1] >= 0xDC00 && wide[index + 1] < 0xE000) {
        code = 0x10000 + ((code - 0xD800) << 10) + (wide[++index] - 0xDC00);
      } else if (code >= 0xD800 && code < 0xE000) {
        code = 0xFFFD;
      }
      if (code < 0x80) {
        name.push_back(static_cast<char>(code));
      } else if (code < 0x800) {
        name.push_back(static_cast<char>(0xC0 | (code >> 6)));
        name.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      } else if (code < 0x10000) {
        name.push_back(static_cast<char>(0xE0 | (code >> 12)));
        name.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        name.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      } else {
        name.push_back(static_cast<char>(0xF0 | (code >> 18)));
        name.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        name.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        name.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      }
    }
    api->co_task_mem_free(wide);
  }
  return name.empty() ? std::string("hardware encoder") : name;
}

void ShutdownTransform(void* transform) {
  Com shutdown;
  if (transform != nullptr && !Failed(Query(transform, kShutdownIid, &shutdown))) {
    Call<Result>(shutdown.get(), kShutdown);
  }
}

class MediaFoundationEncoder final : public VideoEncoder {
 public:
  MediaFoundationEncoder(const MediaFoundation* api, const AvcSettings& settings)
      : api_(api),
        settings_(settings),
        layout_(PictureLayout::InterleavedFor(settings.width, settings.height)),
        frame_bytes_(static_cast<std::uint32_t>(layout_.width) *
                     static_cast<std::uint32_t>(layout_.height) * 3u / 2u),
        frame_duration_(static_cast<std::int64_t>(
            std::llround(10000000.0 / settings.frame_rate))),
        started_(!Failed(api->startup(kMfVersion, kStartupLite))) {}

  ~MediaFoundationEncoder() override {
    const Apartment apartment(api_);
    pool_.clear();
    codec_.reset();
    events_.reset();
    ShutdownTransform(transform_.get());
    transform_.reset();
    if (activate_) {
      Call<Result>(activate_.get(), kShutdownObject);
    }
    activate_.reset();
    if (started_) {
      api_->shutdown();
    }
  }

  MediaFoundationEncoder(const MediaFoundationEncoder&) = delete;
  MediaFoundationEncoder& operator=(const MediaFoundationEncoder&) = delete;

  bool started() const { return started_; }
  const std::string& name() const { return name_; }

  bool Open(void* activate) {
    name_ = FriendlyName(api_, activate);
    Com transform;
    Result result = Call<Result>(activate, kActivateObject, &kTransformIid,
                                 transform.out());
    const char* step = "activation";
    if (!Failed(result)) {
      transform_ = std::move(transform);
      step = Configure(&result);
    }
    if (step != nullptr) {
      std::fprintf(stderr, "  [mediacodec] %s refused the %s: 0x%08x\n",
                   name_.c_str(), step, static_cast<unsigned>(result));
    }
    if (step != nullptr || !Warm()) {
      Close(activate);
      return false;
    }
    Call<std::uint32_t>(activate, kAddRef);
    activate_ = Com(activate);
    return true;
  }

  std::int32_t width() const override { return settings_.width; }
  std::int32_t height() const override { return settings_.height; }
  const PictureLayout& layout() const override { return layout_; }

  bool Encode(const std::uint8_t* picture, std::int64_t time_us,
              std::vector<EncodedPacket>* out) override {
    EnterApartment(api_);
    return !interrupted_.load(std::memory_order_acquire) &&
           Submit(picture, time_us, out);
  }

  bool Drain(std::vector<EncodedPacket>* out) override {
    EnterApartment(api_);
    return !interrupted_.load(std::memory_order_acquire) && Finish(out);
  }

  void RequestKeyFrame() override {
    key_frame_.store(true, std::memory_order_release);
  }

  void Interrupt() override {
    if (interrupted_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    const Apartment apartment(api_);
    ShutdownTransform(transform_.get());
  }

  MediaFormat OutputFormat() const override {
    return AvcOutputFormat(settings_, layout_, sps_, pps_);
  }

 private:
  struct Pooled {
    Com sample;
    Com buffer;
  };

  bool Warm() {
    std::vector<std::uint8_t> black(layout_.Size(), 128);
    std::memset(black.data(), 16, layout_.LumaSize());
    std::vector<EncodedPacket> packets;
    if (!Submit(black.data(), MonotonicUs(), &packets) || !Finish(&packets)) {
      return false;
    }
    const bool keyed =
        config_sent_ &&
        std::any_of(packets.begin(), packets.end(), [](const EncodedPacket& packet) {
          return (packet.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
        });
    if (!keyed) {
      return Fail("test frame, no key frame came out", 0);
    }
    credits_ = 0;
    drained_ = false;
    config_sent_ = false;
    sps_.clear();
    pps_.clear();
    key_frame_.store(true, std::memory_order_release);
    const Result restarted = Message(kMessageStartOfStream);
    return !Failed(restarted) || Fail("restart", restarted);
  }

  void Close(void* activate) {
    pool_.clear();
    codec_.reset();
    events_.reset();
    ShutdownTransform(transform_.get());
    transform_.reset();
    Call<Result>(activate, kShutdownObject);
    input_stream_ = 0;
    output_stream_ = 0;
    provides_samples_ = true;
    output_size_ = 0;
    credits_ = 0;
    drained_ = false;
    broken_ = false;
    key_frame_.store(false, std::memory_order_release);
    sps_.clear();
    pps_.clear();
    config_sent_ = false;
  }

  bool Submit(const std::uint8_t* picture, std::int64_t time_us,
              std::vector<EncodedPacket>* out) {
    if (broken_) {
      return false;
    }
    Pooled input = TakeInput();
    Result result = 0;
    if (!input.sample) {
      return Fail("input sample", result);
    }
    if (!Fill(input.buffer.get(), picture, &result)) {
      return Fail("input copy", result);
    }
    if (Failed(result = Call<Result>(input.sample.get(), kSetSampleTime,
                                     static_cast<std::int64_t>(time_us * 10))) ||
        Failed(result = Call<Result>(input.sample.get(), kSetSampleDuration,
                                     frame_duration_))) {
      return Fail("input time", result);
    }
    if (key_frame_.exchange(false, std::memory_order_acq_rel)) {
      SetCodecValue(kForceKeyFrame, 1);
    }
    while (true) {
      while (credits_ == 0) {
        if (!Pump(true, out, nullptr)) {
          return false;
        }
      }
      const Result accepted = Call<Result>(transform_.get(), kProcessInput,
                                           input_stream_, input.sample.get(),
                                           std::uint32_t{0});
      if (accepted == kNotAccepting) {
        credits_ = 0;
        continue;
      }
      if (Failed(accepted)) {
        return Fail("input", accepted);
      }
      --credits_;
      break;
    }
    if (pool_.size() < kPooledSamples) {
      pool_.push_back(std::move(input));
    }
    bool idle = false;
    while (!idle) {
      if (!Pump(false, out, &idle)) {
        return false;
      }
    }
    return true;
  }

  bool Finish(std::vector<EncodedPacket>* out) {
    if (broken_) {
      return false;
    }
    Result result = 0;
    if (Failed(result = Message(kMessageEndOfStream)) ||
        Failed(result = Message(kMessageDrain))) {
      return Fail("drain", result);
    }
    drained_ = false;
    while (!drained_) {
      if (!Pump(true, out, nullptr)) {
        return false;
      }
    }
    return true;
  }

  bool Fail(const char* step, Result result) {
    if (!broken_ && !interrupted_.load(std::memory_order_acquire)) {
      std::fprintf(stderr, "  [mediacodec] %s failed at the %s: 0x%08x\n",
                   name_.c_str(), step, static_cast<unsigned>(result));
    }
    broken_ = true;
    return false;
  }

  Result Message(std::uint32_t message) {
    return Call<Result>(transform_.get(), kProcessMessage, message,
                        std::uintptr_t{0});
  }

  bool Describe(void* type, const Guid& subtype) const {
    const float rate = settings_.frame_rate;
    const bool whole = std::fabs(rate - std::round(rate)) < 0.001f;
    const auto numerator = static_cast<std::uint32_t>(
        whole ? std::lround(rate) : std::lround(rate * 1000.0f));
    const std::uint32_t denominator = whole ? 1u : 1000u;
    return !Failed(SetGuid(type, kMajorType, kMediaTypeVideo)) &&
           !Failed(SetGuid(type, kSubtype, subtype)) &&
           !Failed(SetUint64(type, kFrameSize,
                             Pack(static_cast<std::uint32_t>(layout_.width),
                                  static_cast<std::uint32_t>(layout_.height)))) &&
           !Failed(SetUint64(type, kFrameRate, Pack(numerator, denominator))) &&
           !Failed(SetUint64(type, kPixelAspectRatio, Pack(1, 1))) &&
           !Failed(SetUint32(type, kInterlaceMode, kProgressive)) &&
           !Failed(SetUint32(type, kVideoPrimaries, kPrimariesBt709)) &&
           !Failed(SetUint32(type, kTransferFunction, kTransfer709)) &&
           !Failed(SetUint32(type, kYuvMatrix, kMatrixBt709)) &&
           !Failed(SetUint32(type, kNominalRange, kRange16To235));
  }

  const char* Configure(Result* result) {
    Com attributes;
    std::uint32_t async = 0;
    *result = Call<Result>(transform_.get(), kGetAttributes, attributes.out());
    if (Failed(*result) ||
        Failed(*result = Call<Result>(attributes.get(), kGetUint32,
                                      &kTransformAsync, &async)) ||
        async == 0 ||
        Failed(*result = SetUint32(attributes.get(), kTransformAsyncUnlock, 1))) {
      return "async unlock";
    }
    SetUint32(attributes.get(), kLowLatency, 1);
    if (Failed(*result = Query(transform_.get(), kEventGeneratorIid, &events_))) {
      return "event generator";
    }
    Query(transform_.get(), kCodecApiIid, &codec_);
    std::uint32_t input_ids[1] = {0};
    std::uint32_t output_ids[1] = {0};
    if (!Failed(Call<Result>(transform_.get(), kGetStreamIds, std::uint32_t{1},
                             input_ids, std::uint32_t{1}, output_ids))) {
      input_stream_ = input_ids[0];
      output_stream_ = output_ids[0];
    }
    if (codec_) {
      SetCodecValue(kRateControlMode, kConstantBitrate);
      SetCodecValue(kMeanBitrate, static_cast<std::uint32_t>(settings_.bitrate));
      SetCodecValue(kBPictureCount, 0);
      SetCodecFlag(kLowLatencyMode);
      SetCodecFlag(kCabacEnable);
      if (settings_.key_frame_interval >= 0) {
        SetCodecValue(kGopSize,
                      static_cast<std::uint32_t>(std::max(
                          1L, std::lround(settings_.key_frame_interval *
                                          settings_.frame_rate))));
      }
    }
    Com output_type;
    if (Failed(*result = api_->create_media_type(output_type.out())) ||
        !Describe(output_type.get(), kFormatH264) ||
        Failed(*result = SetUint32(output_type.get(), kAverageBitrate,
                                   static_cast<std::uint32_t>(settings_.bitrate))) ||
        Failed(*result = SetUint32(output_type.get(), kMpeg2Profile, kProfileHigh)) ||
        Failed(*result = Call<Result>(transform_.get(), kSetOutputType,
                                      output_stream_, output_type.get(),
                                      std::uint32_t{0}))) {
      return "H.264 output type";
    }
    Com input_type;
    if (Failed(*result = api_->create_media_type(input_type.out())) ||
        !Describe(input_type.get(), kFormatNv12) ||
        Failed(*result = Call<Result>(transform_.get(), kSetInputType,
                                      input_stream_, input_type.get(),
                                      std::uint32_t{0}))) {
      return "NV12 input type";
    }
    if (!ReadOutputStreamInfo(result)) {
      return "output stream info";
    }
    if (Failed(*result = Message(kMessageBeginStreaming)) ||
        Failed(*result = Message(kMessageStartOfStream))) {
      return "stream start";
    }
    return nullptr;
  }

  bool ReadOutputStreamInfo(Result* result) {
    OutputStreamInfo info{};
    *result = Call<Result>(transform_.get(), kGetOutputStreamInfo,
                           output_stream_, &info);
    if (Failed(*result)) {
      return false;
    }
    provides_samples_ = (info.flags & (kProvidesSamples | kCanProvideSamples)) != 0;
    output_size_ = std::max<std::uint32_t>(info.size, frame_bytes_);
    return true;
  }

  void SetCodecValue(const Guid& key, std::uint32_t value) {
    Variant variant{};
    variant.type = kVariantUint32;
    variant.value.uint32 = value;
    Call<Result>(codec_.get(), kCodecSetValue, &key, &variant);
  }

  void SetCodecFlag(const Guid& key) {
    Variant variant{};
    variant.type = kVariantBool;
    variant.value.boolean = kVariantTrue;
    Call<Result>(codec_.get(), kCodecSetValue, &key, &variant);
  }

  Pooled NewSample(std::uint32_t bytes) {
    Pooled created;
    if (Failed(api_->create_sample(created.sample.out())) ||
        Failed(api_->create_memory_buffer(bytes, created.buffer.out())) ||
        Failed(Call<Result>(created.sample.get(), kAddBuffer,
                            created.buffer.get()))) {
      return {};
    }
    return created;
  }

  Pooled TakeInput() {
    for (auto entry = pool_.begin(); entry != pool_.end(); ++entry) {
      if (Idle(entry->sample.get(), 1) && Idle(entry->buffer.get(), 2)) {
        Pooled taken = std::move(*entry);
        pool_.erase(entry);
        return taken;
      }
    }
    return NewSample(frame_bytes_);
  }

  bool Fill(void* buffer, const std::uint8_t* picture, Result* result) {
    std::uint8_t* data = nullptr;
    std::uint32_t capacity = 0;
    if (Failed(*result = Call<Result>(buffer, kBufferLock, &data, &capacity,
                                      static_cast<std::uint32_t*>(nullptr))) ||
        data == nullptr) {
      return false;
    }
    const bool fits = capacity >= frame_bytes_;
    if (fits) {
      const auto width = static_cast<std::size_t>(layout_.width);
      const auto rows = static_cast<std::size_t>(layout_.height);
      const std::uint8_t* chroma = picture + layout_.LumaSize();
      std::uint8_t* const target_chroma = data + width * rows;
      if (static_cast<std::size_t>(layout_.luma_stride) == width) {
        std::memcpy(data, picture, width * rows);
        std::memcpy(target_chroma, chroma, width * rows / 2);
      } else {
        for (std::size_t row = 0; row < rows; ++row) {
          std::memcpy(data + row * width, picture + row * layout_.luma_stride,
                      width);
        }
        for (std::size_t row = 0; row < rows / 2; ++row) {
          std::memcpy(target_chroma + row * width,
                      chroma + row * layout_.chroma_stride, width);
        }
      }
    }
    Call<Result>(buffer, kBufferUnlock);
    return fits && !Failed(*result = Call<Result>(
                               buffer, kBufferSetCurrentLength, frame_bytes_));
  }

  bool Pump(bool wait, std::vector<EncodedPacket>* out, bool* idle) {
    Com event;
    const Result got = Call<Result>(events_.get(), kGetEvent,
                                    wait ? std::uint32_t{0} : kEventNoWait,
                                    event.out());
    if (got == kNoEventsAvailable && idle != nullptr) {
      *idle = true;
      return true;
    }
    if (Failed(got)) {
      return Fail("event wait", got);
    }
    std::uint32_t type = 0;
    Result status = 0;
    Result read = Call<Result>(event.get(), kEventGetType, &type);
    if (!Failed(read)) {
      read = Call<Result>(event.get(), kEventGetStatus, &status);
    }
    if (Failed(read)) {
      return Fail("event", read);
    }
    if (Failed(status) || type == kEventError) {
      return Fail("encoder error event", status);
    }
    if (type == kEventNeedInput) {
      ++credits_;
    } else if (type == kEventHaveOutput) {
      return Collect(out);
    } else if (type == kEventDrainComplete) {
      drained_ = true;
    }
    return true;
  }

  bool Collect(std::vector<EncodedPacket>* out) {
    Pooled provided;
    if (!provides_samples_ && !(provided = NewSample(output_size_)).sample) {
      return Fail("output sample", 0);
    }
    OutputBuffer buffer{output_stream_, provided.sample.get(), 0, nullptr};
    std::uint32_t status = 0;
    const Result produced = Call<Result>(transform_.get(), kProcessOutput,
                                         std::uint32_t{0}, std::uint32_t{1},
                                         &buffer, &status);
    if (buffer.events != nullptr) {
      Release(buffer.events);
    }
    Com owned(provides_samples_ ? buffer.sample : nullptr);
    if (produced == kNeedMoreInput) {
      return true;
    }
    if (produced == kStreamChange) {
      Com type;
      Result result = 0;
      if (Failed(result = Call<Result>(transform_.get(), kGetOutputAvailableType,
                                       output_stream_, std::uint32_t{0},
                                       type.out())) ||
          Failed(result = Call<Result>(transform_.get(), kSetOutputType,
                                       output_stream_, type.get(),
                                       std::uint32_t{0})) ||
          !ReadOutputStreamInfo(&result)) {
        return Fail("output type change", result);
      }
      return true;
    }
    if (Failed(produced)) {
      return Fail("output", produced);
    }
    return Emit(provides_samples_ ? owned.get() : provided.sample.get(), out);
  }

  bool Emit(void* sample, std::vector<EncodedPacket>* out) {
    std::int64_t time = 0;
    Com contiguous;
    std::uint8_t* data = nullptr;
    std::uint32_t length = 0;
    Result result = 0;
    if (sample == nullptr ||
        Failed(result = Call<Result>(sample, kGetSampleTime, &time)) ||
        Failed(result = Call<Result>(sample, kConvertToContiguousBuffer,
                                     contiguous.out())) ||
        Failed(result = Call<Result>(contiguous.get(), kBufferLock, &data,
                                     static_cast<std::uint32_t*>(nullptr),
                                     &length))) {
      return Fail("output sample", result);
    }
    EncodedPacket frame;
    frame.time_us = time / 10;
    bool key = false;
    const std::size_t size = length;
    std::size_t start = FindStartCode(data, size, 0);
    while (start < size) {
      const std::size_t begin = start + 3;
      const std::size_t next = FindStartCode(data, size, begin);
      std::size_t end = next;
      while (end > begin && data[end - 1] == 0) {
        --end;
      }
      if (end > begin) {
        const int type = data[begin] & 0x1f;
        if (type == 7 || type == 8) {
          std::vector<std::uint8_t>& kept = type == 7 ? sps_ : pps_;
          if (!config_sent_ && kept.empty()) {
            AppendNal(&kept, data + begin, end - begin);
            if (type == 7) {
              SignalBt709Limited(&kept);
            }
          }
        } else if (type != 9) {
          key = key || type == 5;
          AppendNal(&frame.data, data + begin, end - begin);
        }
      }
      start = next;
    }
    Call<Result>(contiguous.get(), kBufferUnlock);
    if (!config_sent_) {
      if (sps_.empty() || pps_.empty()) {
        return Fail("first frame, no SPS/PPS in it", 0);
      }
      EncodedPacket config;
      config.data = sps_;
      config.data.insert(config.data.end(), pps_.begin(), pps_.end());
      config.time_us = frame.time_us;
      config.flags = AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
      out->push_back(std::move(config));
      config_sent_ = true;
    }
    if (!frame.data.empty()) {
      frame.flags = key ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0;
      out->push_back(std::move(frame));
    }
    return true;
  }

  const MediaFoundation* const api_;
  const AvcSettings settings_;
  const PictureLayout layout_;
  const std::uint32_t frame_bytes_;
  const std::int64_t frame_duration_;
  const bool started_;
  std::string name_;
  Com activate_;
  Com transform_;
  Com events_;
  Com codec_;
  std::uint32_t input_stream_ = 0;
  std::uint32_t output_stream_ = 0;
  bool provides_samples_ = true;
  std::uint32_t output_size_ = 0;
  std::uint32_t credits_ = 0;
  bool drained_ = false;
  bool broken_ = false;
  std::atomic<bool> key_frame_{false};
  std::atomic<bool> interrupted_{false};
  std::vector<Pooled> pool_;
  std::vector<std::uint8_t> sps_;
  std::vector<std::uint8_t> pps_;
  bool config_sent_ = false;
};

}  // namespace

std::unique_ptr<VideoEncoder> CreateHardwareAvcEncoder(
    const AvcSettings& settings) {
  const MediaFoundation* api = Api();
  if (api == nullptr) {
    return nullptr;
  }
  const Apartment apartment(api);
  std::unique_ptr<MediaFoundationEncoder> encoder(
      new (std::nothrow) MediaFoundationEncoder(api, settings));
  if (encoder == nullptr || !encoder->started()) {
    return nullptr;
  }
  const TypeInfo input{kMediaTypeVideo, kFormatNv12};
  const TypeInfo output{kMediaTypeVideo, kFormatH264};
  void** activates = nullptr;
  std::uint32_t count = 0;
  bool opened = false;
  if (!Failed(api->enumerate(kVideoEncoderCategory,
                             kEnumHardware | kEnumSortAndFilter, &input,
                             &output, &activates, &count)) &&
      activates != nullptr) {
    for (std::uint32_t index = 0; index < count; ++index) {
      opened = opened || encoder->Open(activates[index]);
      Release(activates[index]);
    }
    api->co_task_mem_free(activates);
  }
  if (!opened) {
    std::fprintf(stderr,
                 "  [mediacodec] no hardware H.264 encoder took %dx%d, "
                 "encoding on the CPU\n",
                 encoder->layout().width, encoder->layout().height);
    return nullptr;
  }
  std::fprintf(stderr, "  [mediacodec] H.264 on %s\n", encoder->name().c_str());
  return encoder;
}

}  // namespace blox7::media
