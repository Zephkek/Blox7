#ifndef BLOX7_MEDIA_AVC_SPS_H_
#define BLOX7_MEDIA_AVC_SPS_H_

#include <cstdint>
#include <vector>

namespace blox7::media {

bool SignalBt709Limited(std::vector<std::uint8_t>* sps);

}  // namespace blox7::media

#endif  // BLOX7_MEDIA_AVC_SPS_H_
