#ifndef BLOX7_RUNTIME_ALBUM_SAVER_H_
#define BLOX7_RUNTIME_ALBUM_SAVER_H_

#include <string>

namespace blox7::runtime {

bool SaveToWindowsMediaFolder(const std::string& path, const std::string& name,
                              std::string* message);
bool SaveCaptureToWindowsMediaFolder(const std::string& path,
                                     std::string* message);
void MoveFinishedVideos(const std::string& directory);

}  // namespace blox7::runtime

#endif  // BLOX7_RUNTIME_ALBUM_SAVER_H_
