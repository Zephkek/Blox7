#include "media/media_format.h"

#include <cinttypes>
#include <cstdio>

namespace blox7::media {

void MediaFormat::Set(const char* key, Value value) {
  for (auto& [name, current] : entries_) {
    if (name == key) {
      current = std::move(value);
      return;
    }
  }
  entries_.emplace_back(key, std::move(value));
}

const MediaFormat::Value* MediaFormat::Find(const char* key) const {
  for (const auto& [name, value] : entries_) {
    if (name == key) {
      return &value;
    }
  }
  return nullptr;
}

bool MediaFormat::GetInt32(const char* key, std::int32_t* out) const {
  const Value* value = Find(key);
  if (value == nullptr || !std::holds_alternative<std::int32_t>(*value)) {
    return false;
  }
  *out = std::get<std::int32_t>(*value);
  return true;
}

const MediaFormat::Buffer* MediaFormat::GetBuffer(const char* key) const {
  const Value* value = Find(key);
  return value != nullptr ? std::get_if<Buffer>(value) : nullptr;
}

std::string MediaFormat::ToString() const {
  std::string text;
  char number[64];
  for (const auto& [name, value] : entries_) {
    if (!text.empty()) {
      text += ", ";
    }
    text += name;
    text += ": ";
    if (const auto* i32 = std::get_if<std::int32_t>(&value)) {
      std::snprintf(number, sizeof number, "int32(%" PRId32 ")", *i32);
      text += number;
    } else if (const auto* i64 = std::get_if<std::int64_t>(&value)) {
      std::snprintf(number, sizeof number, "int64(%" PRId64 ")", *i64);
      text += number;
    } else if (const auto* f = std::get_if<float>(&value)) {
      std::snprintf(number, sizeof number, "float(%f)", *f);
      text += number;
    } else if (const auto* s = std::get_if<std::string>(&value)) {
      text += "string(" + *s + ")";
    } else {
      text += "data";
    }
  }
  return text;
}

}  // namespace blox7::media
