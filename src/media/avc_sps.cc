#include "media/avc_sps.h"

#include <cstddef>
#include <utility>

namespace blox7::media {
namespace {

constexpr std::uint32_t kBt709 = 1;
constexpr std::uint32_t kUnspecifiedVideoFormat = 5;
constexpr std::size_t kNoBit = static_cast<std::size_t>(-1);

class BitReader {
 public:
  explicit BitReader(const std::vector<std::uint8_t>& data) : data_(data) {}

  bool Bits(int count, std::uint32_t* value) {
    std::uint32_t result = 0;
    for (int index = 0; index < count; ++index) {
      if (position_ >= data_.size() * 8) {
        return false;
      }
      result = (result << 1) |
               ((data_[position_ / 8] >> (7 - position_ % 8)) & 1u);
      ++position_;
    }
    *value = result;
    return true;
  }
  bool Bit(std::uint32_t* value) { return Bits(1, value); }
  bool Ue(std::uint32_t* value) {
    int zeros = 0;
    std::uint32_t bit = 0;
    while (true) {
      if (!Bit(&bit)) {
        return false;
      }
      if (bit != 0) {
        break;
      }
      if (++zeros > 31) {
        return false;
      }
    }
    std::uint32_t rest = 0;
    if (!Bits(zeros, &rest)) {
      return false;
    }
    *value = ((1u << zeros) - 1u) + rest;
    return true;
  }
  bool Se(std::int32_t* value) {
    std::uint32_t code = 0;
    if (!Ue(&code)) {
      return false;
    }
    *value = (code & 1u) != 0 ? static_cast<std::int32_t>((code + 1u) / 2u)
                              : -static_cast<std::int32_t>(code / 2u);
    return true;
  }
  std::size_t position() const { return position_; }

 private:
  const std::vector<std::uint8_t>& data_;
  std::size_t position_ = 0;
};

class BitWriter {
 public:
  void Bit(std::uint32_t bit) {
    if (used_ % 8 == 0) {
      data_.push_back(0);
    }
    if (bit != 0) {
      data_.back() |= static_cast<std::uint8_t>(0x80u >> (used_ % 8));
    }
    ++used_;
  }
  void Bits(std::uint32_t value, int count) {
    for (int index = count - 1; index >= 0; --index) {
      Bit((value >> index) & 1u);
    }
  }
  void Copy(const std::vector<std::uint8_t>& source, std::size_t from,
            std::size_t to) {
    for (std::size_t position = from; position < to; ++position) {
      Bit((source[position / 8] >> (7 - position % 8)) & 1u);
    }
  }
  std::vector<std::uint8_t> Finish() {
    Bit(1);
    while (used_ % 8 != 0) {
      Bit(0);
    }
    return std::move(data_);
  }

 private:
  std::vector<std::uint8_t> data_;
  std::size_t used_ = 0;
};

std::vector<std::uint8_t> Unescape(const std::uint8_t* data, std::size_t size) {
  std::vector<std::uint8_t> rbsp;
  rbsp.reserve(size);
  int zeros = 0;
  for (std::size_t index = 0; index < size; ++index) {
    if (zeros >= 2 && data[index] == 3) {
      zeros = 0;
      continue;
    }
    rbsp.push_back(data[index]);
    zeros = data[index] == 0 ? zeros + 1 : 0;
  }
  return rbsp;
}

std::vector<std::uint8_t> Escape(const std::vector<std::uint8_t>& rbsp) {
  std::vector<std::uint8_t> ebsp;
  ebsp.reserve(rbsp.size() + rbsp.size() / 64 + 1);
  int zeros = 0;
  for (const std::uint8_t byte : rbsp) {
    if (zeros >= 2 && byte <= 3) {
      ebsp.push_back(3);
      zeros = 0;
    }
    ebsp.push_back(byte);
    zeros = byte == 0 ? zeros + 1 : 0;
  }
  return ebsp;
}

std::size_t StopBit(const std::vector<std::uint8_t>& rbsp) {
  for (std::size_t index = rbsp.size(); index-- > 0;) {
    const std::uint8_t byte = rbsp[index];
    if (byte != 0) {
      int low = 0;
      while (((byte >> low) & 1u) == 0) {
        ++low;
      }
      return index * 8 + static_cast<std::size_t>(7 - low);
    }
  }
  return kNoBit;
}

bool HighProfile(std::uint32_t profile) {
  switch (profile) {
    case 44:
    case 83:
    case 86:
    case 100:
    case 110:
    case 118:
    case 122:
    case 128:
    case 134:
    case 135:
    case 138:
    case 139:
    case 244:
      return true;
    default:
      return false;
  }
}

bool SkipScalingLists(BitReader* reader, int lists) {
  for (int list = 0; list < lists; ++list) {
    std::uint32_t present = 0;
    if (!reader->Bit(&present)) {
      return false;
    }
    if (present == 0) {
      continue;
    }
    const int size = list < 6 ? 16 : 64;
    std::int32_t last = 8;
    std::int32_t next = 8;
    for (int entry = 0; entry < size; ++entry) {
      if (next != 0) {
        std::int32_t delta = 0;
        if (!reader->Se(&delta)) {
          return false;
        }
        next = (last + delta + 256) % 256;
      }
      last = next == 0 ? last : next;
    }
  }
  return true;
}

bool SkipToVui(BitReader* reader) {
  std::uint32_t profile = 0;
  std::uint32_t value = 0;
  if (!reader->Bits(8, &profile) || !reader->Bits(16, &value) ||
      !reader->Ue(&value)) {
    return false;
  }
  if (HighProfile(profile)) {
    std::uint32_t chroma_format = 0;
    std::uint32_t scaling = 0;
    if (!reader->Ue(&chroma_format) ||
        (chroma_format == 3 && !reader->Bit(&value)) || !reader->Ue(&value) ||
        !reader->Ue(&value) || !reader->Bit(&value) || !reader->Bit(&scaling) ||
        (scaling != 0 &&
         !SkipScalingLists(reader, chroma_format == 3 ? 12 : 8))) {
      return false;
    }
  }
  std::uint32_t order_type = 0;
  if (!reader->Ue(&value) || !reader->Ue(&order_type)) {
    return false;
  }
  if (order_type == 0) {
    if (!reader->Ue(&value)) {
      return false;
    }
  } else if (order_type == 1) {
    std::int32_t offset = 0;
    std::uint32_t cycle = 0;
    if (!reader->Bit(&value) || !reader->Se(&offset) || !reader->Se(&offset) ||
        !reader->Ue(&cycle) || cycle > 255) {
      return false;
    }
    for (std::uint32_t frame = 0; frame < cycle; ++frame) {
      if (!reader->Se(&offset)) {
        return false;
      }
    }
  }
  std::uint32_t frame_mbs_only = 0;
  std::uint32_t cropping = 0;
  if (!reader->Ue(&value) || !reader->Bit(&value) || !reader->Ue(&value) ||
      !reader->Ue(&value) || !reader->Bit(&frame_mbs_only) ||
      (frame_mbs_only == 0 && !reader->Bit(&value)) || !reader->Bit(&value) ||
      !reader->Bit(&cropping)) {
    return false;
  }
  for (int edge = 0; cropping != 0 && edge < 4; ++edge) {
    if (!reader->Ue(&value)) {
      return false;
    }
  }
  return true;
}

void WriteSignal(BitWriter* writer, std::uint32_t video_format) {
  writer->Bit(1);
  writer->Bits(video_format, 3);
  writer->Bit(0);
  writer->Bit(1);
  writer->Bits(kBt709, 8);
  writer->Bits(kBt709, 8);
  writer->Bits(kBt709, 8);
}

}  // namespace

bool SignalBt709Limited(std::vector<std::uint8_t>* sps) {
  std::size_t header = 0;
  if (sps->size() >= 4 && (*sps)[0] == 0 && (*sps)[1] == 0 &&
      (*sps)[2] == 0 && (*sps)[3] == 1) {
    header = 4;
  } else if (sps->size() >= 3 && (*sps)[0] == 0 && (*sps)[1] == 0 &&
             (*sps)[2] == 1) {
    header = 3;
  }
  if (sps->size() < header + 4 || ((*sps)[header] & 0x1f) != 7) {
    return false;
  }
  const std::vector<std::uint8_t> rbsp =
      Unescape(sps->data() + header + 1, sps->size() - header - 1);
  BitReader reader(rbsp);
  if (!SkipToVui(&reader)) {
    return false;
  }
  const std::size_t vui_flag = reader.position();
  std::uint32_t vui = 0;
  if (!reader.Bit(&vui)) {
    return false;
  }
  BitWriter writer;
  if (vui == 0) {
    writer.Copy(rbsp, 0, vui_flag);
    writer.Bit(1);
    writer.Bit(0);
    writer.Bit(0);
    WriteSignal(&writer, kUnspecifiedVideoFormat);
    for (int flag = 0; flag < 6; ++flag) {
      writer.Bit(0);
    }
  } else {
    std::uint32_t present = 0;
    std::uint32_t value = 0;
    if (!reader.Bit(&present) ||
        (present != 0 && (!reader.Bits(8, &value) ||
                          (value == 255 && (!reader.Bits(16, &value) ||
                                            !reader.Bits(16, &value))))) ||
        !reader.Bit(&present) || (present != 0 && !reader.Bit(&value))) {
      return false;
    }
    const std::size_t signal = reader.position();
    std::uint32_t video_format = kUnspecifiedVideoFormat;
    std::uint32_t full_range = 0;
    std::uint32_t described = 0;
    std::uint32_t primaries = 0;
    std::uint32_t transfer = 0;
    std::uint32_t matrix = 0;
    if (!reader.Bit(&present) ||
        (present != 0 &&
         (!reader.Bits(3, &video_format) || !reader.Bit(&full_range) ||
          !reader.Bit(&described) ||
          (described != 0 &&
           (!reader.Bits(8, &primaries) || !reader.Bits(8, &transfer) ||
            !reader.Bits(8, &matrix)))))) {
      return false;
    }
    if (present != 0 && full_range == 0 && described != 0 &&
        primaries == kBt709 && transfer == kBt709 && matrix == kBt709) {
      return true;
    }
    const std::size_t rest = reader.position();
    const std::size_t stop = StopBit(rbsp);
    if (stop == kNoBit || stop < rest) {
      return false;
    }
    writer.Copy(rbsp, 0, signal);
    WriteSignal(&writer, video_format);
    writer.Copy(rbsp, rest, stop);
  }
  const std::vector<std::uint8_t> rebuilt = Escape(writer.Finish());
  sps->resize(header + 1);
  sps->insert(sps->end(), rebuilt.begin(), rebuilt.end());
  return true;
}

}  // namespace blox7::media
