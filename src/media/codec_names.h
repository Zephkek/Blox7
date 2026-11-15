#ifndef BLOX7_MEDIA_CODEC_NAMES_H_
#define BLOX7_MEDIA_CODEC_NAMES_H_

#include <string>

namespace blox7::media {

enum class CodecKind { kAvc, kHevc, kAac };

bool KindForMime(const char* mime, CodecKind* kind);
bool KindForName(const char* name, CodecKind* kind);
const char* CodecMime(CodecKind kind);
std::string CodecName(CodecKind kind);

}  // namespace blox7::media

#endif  // BLOX7_MEDIA_CODEC_NAMES_H_
