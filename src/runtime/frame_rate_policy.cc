#include "runtime/frame_rate_policy.h"

#define JSON_NOEXCEPTION 1
#include <nlohmann/json.hpp>

#include <charconv>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>

namespace blox7 {
namespace runtime {
namespace {

constexpr std::string_view kFramerateCapOpen = "<int name=\"FramerateCap\">";

bool SetCompatibleValue(nlohmann::json *object, const char *key,
                        const std::string &value, std::string *error) {
  const auto existing = object->find(key);
  if (existing != object->end()) {
    if (!existing->is_string() || existing->get<std::string>() != value) {
      if (error != nullptr) {
        *error = std::string("frame-rate policy conflicts with ") + key;
      }
      return false;
    }
    return true;
  }
  (*object)[key] = value;
  return true;
}

bool ParseInt(std::string_view text, int *value) {
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), *value);
  return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}

std::filesystem::path BasicSettingsFile(const std::filesystem::path &app_data) {
  constexpr std::string_view kPrefix = "GlobalBasicSettings_";
  constexpr std::string_view kSuffix = ".xml";
  std::filesystem::path newest;
  int newest_version = -1;
  std::error_code error;
  for (std::filesystem::directory_iterator it(app_data, error), end;
       !error && it != end; it.increment(error)) {
    const std::string name = it->path().filename().string();
    if (name.size() <= kPrefix.size() + kSuffix.size() ||
        name.compare(0, kPrefix.size(), kPrefix) != 0 ||
        name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) !=
            0) {
      continue;
    }
    int version = 0;
    if (ParseInt(std::string_view(name).substr(
                     kPrefix.size(),
                     name.size() - kPrefix.size() - kSuffix.size()),
                 &version) &&
        version > newest_version) {
      newest_version = version;
      newest = it->path();
    }
  }
  return newest;
}

bool ReadFile(const std::filesystem::path &file, std::string *text) {
  std::ifstream input(file, std::ios::binary);
  if (!input) {
    return false;
  }
  text->assign(std::istreambuf_iterator<char>(input), {});
  return !input.bad();
}

bool ReplaceFile(const std::filesystem::path &file, const std::string &text,
                 std::string *error) {
  std::filesystem::path temporary = file;
  temporary += ".blox7";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << text;
    output.flush();
    if (!output) {
      *error = "cannot write " + temporary.string();
      return false;
    }
  }
  std::error_code ignored;
  const auto old_status = std::filesystem::status(file, ignored);
  if (std::filesystem::exists(old_status)) {
    std::filesystem::permissions(temporary, old_status.permissions(), ignored);
  }
  std::error_code renamed;
  std::filesystem::rename(temporary, file, renamed);
  if (renamed) {
    std::filesystem::remove(temporary, ignored);
    *error = "cannot replace " + file.string() + ": " + renamed.message();
    return false;
  }
  return true;
}

struct Loan {
  int written = 0;
  int replaced = -1;
};

std::optional<Loan> ReadLoan(const std::filesystem::path &file) {
  std::string text;
  if (!ReadFile(file, &text)) {
    return std::nullopt;
  }
  std::istringstream fields(text);
  Loan loan;
  if (!(fields >> loan.written >> loan.replaced)) {
    return std::nullopt;
  }
  return loan;
}

struct CapField {
  std::size_t start = std::string::npos;
  std::size_t end = std::string::npos;
  std::optional<int> value;
};

CapField FindCap(const std::string &xml) {
  CapField field;
  const std::size_t at = xml.find(kFramerateCapOpen);
  if (at == std::string::npos) {
    return field;
  }
  field.start = at + kFramerateCapOpen.size();
  field.end = xml.find('<', field.start);
  int value = 0;
  if (field.end != std::string::npos &&
      ParseInt(std::string_view(xml).substr(field.start,
                                            field.end - field.start),
               &value)) {
    field.value = value;
  }
  return field;
}

std::optional<std::string> WithCap(const std::filesystem::path &file,
                                   std::string xml, int cap,
                                   std::string *error) {
  const CapField field = FindCap(xml);
  if (field.value) {
    xml.replace(field.start, field.end - field.start, std::to_string(cap));
    return xml;
  }
  if (field.start != std::string::npos) {
    *error = "FramerateCap in " + file.string() + " is not a number";
    return std::nullopt;
  }
  const std::size_t properties = xml.find("<Properties>");
  const std::size_t line = properties == std::string::npos
                               ? std::string::npos
                               : xml.find('\n', properties);
  if (line == std::string::npos) {
    *error = file.string() + " has no Properties";
    return std::nullopt;
  }
  const std::size_t indent_end = xml.find_first_not_of(" \t", line + 1);
  const std::string indent = xml.substr(
      line + 1, indent_end == std::string::npos ? 0 : indent_end - line - 1);
  xml.insert(line + 1, indent + std::string(kFramerateCapOpen) +
                           std::to_string(cap) + "</int>\n");
  return xml;
}

} // namespace

FrameRatePolicy ParseFrameRatePolicy(std::string_view value) {
  if (value.empty() || value == "-1") {
    return {};
  }
  if (value == "display") {
    return {FrameRateLimitMode::kDisplay, 0};
  }
  if (value == "unlimited") {
    return {FrameRateLimitMode::kUnlimited, 0};
  }
  int fps = 0;
  if (ParseInt(value, &fps) && fps > 0) {
    return {FrameRateLimitMode::kFixed, fps};
  }
  return {FrameRateLimitMode::kInvalid, 0};
}

std::optional<int> RobloxFramerateCapFor(const FrameRatePolicy &policy) {
  if (policy.mode == FrameRateLimitMode::kUnlimited) {
    return kUnlimitedRobloxFramerateCap;
  }
  if (policy.mode == FrameRateLimitMode::kFixed &&
      policy.fixed_fps > kMaximumSupportedRobloxSchedulerFps) {
    return policy.fixed_fps;
  }
  return std::nullopt;
}

bool SyncRobloxFramerateCap(const std::filesystem::path &app_data,
                            const std::filesystem::path &loan_file,
                            std::optional<int> cap, std::string *error) {
  const std::optional<Loan> loan = ReadLoan(loan_file);
  const std::filesystem::path settings = BasicSettingsFile(app_data);
  std::string xml;
  if (settings.empty() || !ReadFile(settings, &xml)) {
    if (cap) {
      *error = settings.empty() ? "Roblox has not saved its settings yet"
                                : "cannot read " + settings.string();
      return false;
    }
    return true;
  }
  const std::optional<int> current = FindCap(xml).value;
  std::error_code ignored;
  if (!cap) {
    if (!loan) {
      return true;
    }
    if (current == loan->written) {
      const std::optional<std::string> restored =
          WithCap(settings, std::move(xml), loan->replaced, error);
      if (!restored || !ReplaceFile(settings, *restored, error)) {
        return false;
      }
    }
    std::filesystem::remove(loan_file, ignored);
    return true;
  }
  std::optional<std::string> capped;
  if (current != *cap) {
    capped = WithCap(settings, std::move(xml), *cap, error);
    if (!capped) {
      return false;
    }
  }
  const int replaced = loan && current == loan->written ? loan->replaced
                                                        : current.value_or(-1);
  if (!loan || loan->written != *cap || loan->replaced != replaced) {
    std::filesystem::create_directories(loan_file.parent_path(), ignored);
    if (!ReplaceFile(loan_file,
                     std::to_string(*cap) + ' ' + std::to_string(replaced) +
                         '\n',
                     error)) {
      return false;
    }
  }
  return !capped || ReplaceFile(settings, *capped, error);
}

bool MergeFrameRateClientSettingsOverrides(const FrameRatePolicy &policy,
                                           std::string_view base_json,
                                           std::string *merged_json,
                                           std::string *error) {
  if (merged_json == nullptr || !policy.valid()) {
    if (error != nullptr) {
      *error = "invalid frame-rate policy";
    }
    return false;
  }
  nlohmann::json overrides = nlohmann::json::parse(
      base_json.empty() ? "{}" : base_json, nullptr, false, true);
  if (overrides.is_discarded() || !overrides.is_object()) {
    if (error != nullptr) {
      *error = "client-settings overrides must be a JSON object";
    }
    return false;
  }
  if (!SetCompatibleValue(&overrides,
                          "FFlagGameBasicSettingsFramerateCap5", "True",
                          error)) {
    return false;
  }
  if (policy.mode == FrameRateLimitMode::kUnmanaged ||
      policy.mode == FrameRateLimitMode::kDisplay ||
      policy.held_by_roblox_setting) {
    *merged_json = overrides.dump();
    return true;
  }
  const std::string target =
      policy.mode == FrameRateLimitMode::kUnlimited
          ? std::to_string(kMaximumSupportedRobloxSchedulerFps)
          : std::to_string(policy.fixed_fps);
  if (!SetCompatibleValue(&overrides, "DFIntTaskSchedulerTargetFps", target,
                          error)) {
    return false;
  }
  *merged_json = overrides.dump();
  return true;
}

} // namespace runtime
} // namespace blox7
