#ifndef BLOX7_MEDIA_MEDIA_FORMAT_H_
#define BLOX7_MEDIA_MEDIA_FORMAT_H_

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace blox7::media {

class MediaFormat {
 public:
  using Buffer = std::vector<std::uint8_t>;
  using Value =
      std::variant<std::int32_t, std::int64_t, float, std::string, Buffer>;

  void Set(const char* key, Value value);
  bool GetInt32(const char* key, std::int32_t* out) const;
  bool GetFloat(const char* key, float* out) const;
  std::int32_t Int32Or(const char* key, std::int32_t fallback) const;
  float FloatOr(const char* key, float fallback) const;
  const Buffer* GetBuffer(const char* key) const;
  std::string ToString() const;

 private:
  const Value* Find(const char* key) const;

  std::vector<std::pair<std::string, Value>> entries_;
};

}  // namespace blox7::media

#endif  // BLOX7_MEDIA_MEDIA_FORMAT_H_
