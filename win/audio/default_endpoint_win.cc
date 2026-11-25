#include "blox7/audio/host_default_endpoint.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

extern "C" void* __w32_pe_open(const char* utf8_path);
extern "C" void* __w32_pe_symbol(void* module, const char* name);

namespace blox7::audio {
namespace {

#define BLOX7_MS __attribute__((ms_abi))

struct Guid {
  std::uint32_t a;
  std::uint16_t b, c;
  std::uint8_t d[8];
};

struct PropertyKey {
  Guid id;
  std::uint32_t index;
};

struct PropertyBlob {
  std::uint32_t size;
  const std::uint8_t* data;
};

struct PropertyValue {
  std::uint16_t type;
  std::uint16_t reserved[3];
  union {
    const char16_t* text;
    PropertyBlob blob;
  };
};

using Result = std::int32_t;

constexpr Guid kEnumeratorClass = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
constexpr Guid kEnumeratorInterface = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
constexpr PropertyKey kFriendlyName = {
    {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}},
    14};
constexpr PropertyKey kMixFormat = {
    {0xF19F064D, 0x082C, 0x4E27, {0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C}},
    0};
constexpr std::uint32_t kMultithreaded = 0;
constexpr std::uint32_t kInProcess = 1;
constexpr std::uint32_t kRender = 0;
constexpr std::uint32_t kCapture = 1;
constexpr std::uint32_t kConsoleRole = 0;
constexpr std::uint32_t kRead = 0;
constexpr std::uint16_t kWideText = 31;
constexpr std::uint16_t kBlob = 65;
constexpr std::size_t kSampleRateOffset = 4;
constexpr Result kChangedMode = static_cast<Result>(0x80010106U);
constexpr Result kNotFound = static_cast<Result>(0x80070490U);
constexpr std::size_t kRelease = 2;
constexpr std::size_t kGetDefaultEndpoint = 4;
constexpr std::size_t kOpenPropertyStore = 4;
constexpr std::size_t kGetValue = 5;

using ReleaseFn = std::uint32_t(BLOX7_MS*)(void*);
using GetDefaultEndpointFn = Result(BLOX7_MS*)(void*, std::uint32_t,
                                               std::uint32_t, void**);
using OpenPropertyStoreFn = Result(BLOX7_MS*)(void*, std::uint32_t, void**);
using GetValueFn = Result(BLOX7_MS*)(void*, const PropertyKey*,
                                     PropertyValue*);

struct Ole {
  Result(BLOX7_MS* initialize)(void*, std::uint32_t) = nullptr;
  void(BLOX7_MS* uninitialize)() = nullptr;
  Result(BLOX7_MS* create)(const Guid*, void*, std::uint32_t, const Guid*,
                           void**) = nullptr;
  Result(BLOX7_MS* clear)(PropertyValue*) = nullptr;
};

template <typename Fn>
bool Bind(void* module, const char* name, Fn* slot) {
  *slot = module != nullptr ? reinterpret_cast<Fn>(__w32_pe_symbol(module, name))
                            : nullptr;
  return *slot != nullptr;
}

const Ole* Api() {
  static const std::unique_ptr<Ole> api = [] {
    auto loaded = std::make_unique<Ole>();
    void* ole32 = __w32_pe_open("ole32.dll");
    const bool ok = Bind(ole32, "CoInitializeEx", &loaded->initialize) &&
                    Bind(ole32, "CoUninitialize", &loaded->uninitialize) &&
                    Bind(ole32, "CoCreateInstance", &loaded->create) &&
                    Bind(ole32, "PropVariantClear", &loaded->clear);
    return ok ? std::move(loaded) : nullptr;
  }();
  return api.get();
}

template <typename Fn>
Fn Method(void* object, std::size_t slot) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[slot]);
}

void Release(void* object) {
  if (object != nullptr) {
    Method<ReleaseFn>(object, kRelease)(object);
  }
}

void AppendUtf8(std::uint32_t point, std::string* text) {
  if (point < 0x80) {
    text->push_back(static_cast<char>(point));
  } else if (point < 0x800) {
    text->push_back(static_cast<char>(0xC0 | (point >> 6)));
    text->push_back(static_cast<char>(0x80 | (point & 0x3F)));
  } else if (point < 0x10000) {
    text->push_back(static_cast<char>(0xE0 | (point >> 12)));
    text->push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
    text->push_back(static_cast<char>(0x80 | (point & 0x3F)));
  } else {
    text->push_back(static_cast<char>(0xF0 | (point >> 18)));
    text->push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
    text->push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
    text->push_back(static_cast<char>(0x80 | (point & 0x3F)));
  }
}

std::string Utf8(const char16_t* wide) {
  std::string text;
  for (std::size_t i = 0; wide[i] != 0; ++i) {
    std::uint32_t point = wide[i];
    if (point >= 0xD800 && point < 0xDC00 && wide[i + 1] >= 0xDC00 &&
        wide[i + 1] < 0xE000) {
      point = 0x10000 + ((point - 0xD800) << 10) + (wide[i + 1] - 0xDC00);
      ++i;
    }
    AppendUtf8(point, &text);
  }
  return text;
}

bool ReadEndpoint(const Ole* api, void* endpoint, std::string* name,
                  int* mix_rate_hz) {
  void* store = nullptr;
  if (Method<OpenPropertyStoreFn>(endpoint, kOpenPropertyStore)(
          endpoint, kRead, &store) < 0 ||
      store == nullptr) {
    return false;
  }
  bool read = false;
  PropertyValue value{};
  if (Method<GetValueFn>(store, kGetValue)(store, &kFriendlyName, &value) >=
      0) {
    if (value.type == kWideText && value.text != nullptr) {
      *name = Utf8(value.text);
      read = true;
    }
    api->clear(&value);
  }
  value = PropertyValue{};
  if (Method<GetValueFn>(store, kGetValue)(store, &kMixFormat, &value) >= 0) {
    std::uint32_t rate = 0;
    if (value.type == kBlob && value.blob.data != nullptr &&
        value.blob.size >= kSampleRateOffset + sizeof(rate)) {
      std::memcpy(&rate, value.blob.data + kSampleRateOffset, sizeof(rate));
      *mix_rate_hz = static_cast<int>(rate);
    }
    api->clear(&value);
  }
  Release(store);
  return read;
}

}  // namespace

bool HostDefaultAudioEndpoint(bool recording, std::string* name,
                              int* mix_rate_hz) {
  name->clear();
  *mix_rate_hz = 0;
  const Ole* api = Api();
  if (api == nullptr) {
    return false;
  }
  const Result entered = api->initialize(nullptr, kMultithreaded);
  if (entered < 0 && entered != kChangedMode) {
    return false;
  }
  bool known = false;
  void* enumerator = nullptr;
  if (api->create(&kEnumeratorClass, nullptr, kInProcess,
                  &kEnumeratorInterface, &enumerator) >= 0 &&
      enumerator != nullptr) {
    void* endpoint = nullptr;
    const Result found =
        Method<GetDefaultEndpointFn>(enumerator, kGetDefaultEndpoint)(
            enumerator, recording ? kCapture : kRender, kConsoleRole,
            &endpoint);
    if (found == kNotFound) {
      known = true;
    } else if (found >= 0 && endpoint != nullptr) {
      known = ReadEndpoint(api, endpoint, name, mix_rate_hz);
    }
    Release(endpoint);
    Release(enumerator);
  }
  if (entered >= 0) {
    api->uninitialize();
  }
  return known;
}

}  // namespace blox7::audio
