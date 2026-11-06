#ifndef BLOX7_UPDATE_STATE_FILE_H_
#define BLOX7_UPDATE_STATE_FILE_H_

#include <filesystem>
#include <nlohmann/json.hpp>

namespace blox7::update {

nlohmann::json ReadStateFile(const std::filesystem::path& path);
bool WriteStateFile(const std::filesystem::path& path,
                    const nlohmann::json& document);

}

#endif
