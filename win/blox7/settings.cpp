#include <dxgi1_6.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <map>
#include <optional>

#include "blox7.h"
#include "default_config.h"
#include "forced_flags.h"

namespace blox7 {
namespace {

std::string read_all(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  std::string text;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20)) {
    text.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
    text.resize(got);
  }
  CloseHandle(file);
  return text;
}

void make_parent(const std::wstring& path) {
  for (size_t i = 3; i < path.size(); ++i)
    if (path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    if (end == text.size()) break;
    pos = end + 1;
  }
  if (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

std::string join_lines(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) text += line + "\n";
  return text;
}

std::string trimmed(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
  return s.substr(start);
}

bool indented(const std::string& line) { return !line.empty() && (line[0] == ' ' || line[0] == '\t'); }

bool blank_or_comment(const std::string& line) {
  const std::string t = trimmed(line);
  return t.empty() || t[0] == '#';
}

std::string unquoted(std::string value) {
  const size_t hash = value.find(" #");
  if (hash != std::string::npos) value = trimmed(value.substr(0, hash));
  if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) value = value.substr(1, value.size() - 2);
  return value;
}

// yaml parser speedrun
std::string yaml_get(const std::string& text, const std::string& section, const std::string& key) {
  bool inside = false;
  for (const std::string& line : split_lines(text)) {
    if (!indented(line) && !blank_or_comment(line)) {
      inside = trimmed(line) == section + ":";
      continue;
    }
    if (!inside || blank_or_comment(line)) continue;
    const std::string t = trimmed(line);
    if (t.rfind(key + ":", 0) == 0) return unquoted(trimmed(t.substr(key.size() + 1)));
  }
  return {};
}

std::string yaml_set(const std::string& text, const std::string& section, const std::string& key, const std::string& value) {
  std::vector<std::string> lines = split_lines(text);
  size_t header = lines.size();
  for (size_t i = 0; i < lines.size(); ++i)
    if (!indented(lines[i]) && trimmed(lines[i]) == section + ":") {
      header = i;
      break;
    }
  if (header == lines.size()) {
    lines.push_back("");
    lines.push_back(section + ":");
    lines.push_back("  " + key + ": " + value);
    return join_lines(lines);
  }
  for (size_t i = header + 1; i < lines.size(); ++i) {
    if (!indented(lines[i]) && !blank_or_comment(lines[i])) break;
    if (blank_or_comment(lines[i])) continue;
    const std::string t = trimmed(lines[i]);
    if (t.rfind(key + ":", 0) == 0) {
      lines[i] = lines[i].substr(0, lines[i].find(key)) + key + ": " + value;
      return join_lines(lines);
    }
  }
  lines.insert(lines.begin() + static_cast<long>(header) + 1, "  " + key + ": " + value);
  return join_lines(lines);
}

std::optional<std::string> yaml_top(const std::string& text, const std::string& key) {
  for (const std::string& line : split_lines(text))
    if (!indented(line) && line.rfind(key + ":", 0) == 0) return unquoted(trimmed(line.substr(key.size() + 1)));
  return std::nullopt;
}

std::string yaml_set_top(const std::string& text, const std::string& key, const std::string& value) {
  std::vector<std::string> lines = split_lines(text);
  for (size_t i = 0; i < lines.size(); ++i) {
    if (indented(lines[i]) || lines[i].rfind(key + ":", 0) != 0) continue;
    lines[i] = key + ": " + value;
    size_t end = i + 1;
    while (end < lines.size() && indented(lines[end]) && !blank_or_comment(lines[end])) ++end;
    lines.erase(lines.begin() + static_cast<long>(i) + 1, lines.begin() + static_cast<long>(end));
    return join_lines(lines);
  }
  lines.push_back(key + ": " + value);
  return join_lines(lines);
}

std::string yaml_set_block(const std::string& text, const std::string& key, const std::vector<std::string>& entries) {
  std::vector<std::string> lines = split_lines(text);
  std::vector<std::string> block{key + ":"};
  for (const std::string& entry : entries) block.push_back("  " + entry);
  for (size_t i = 0; i < lines.size(); ++i) {
    if (indented(lines[i]) || lines[i].rfind(key + ":", 0) != 0) continue;
    size_t end = i + 1;
    while (end < lines.size() && indented(lines[end]) && !blank_or_comment(lines[end])) ++end;
    lines.erase(lines.begin() + static_cast<long>(i), lines.begin() + static_cast<long>(end));
    lines.insert(lines.begin() + static_cast<long>(i), block.begin(), block.end());
    return join_lines(lines);
  }
  lines.insert(lines.end(), block.begin(), block.end());
  return join_lines(lines);
}

// phones as the real app sees them
struct Phone {
  const wchar_t* id;
  const wchar_t* name;
  const char* manufacturer;
  const char* model;
  const char* brand;
  const char* code;
  const char* soc;
  int ram, width, height, xdpi, ydpi, dp_width, dp_height, android;
};

const Phone kPhones[] = {
    {L"phone-pixel-7", L"Google Pixel 7", "Google", "Pixel 7", "google", "panther",
     "Google Tensor G2", 7581, 1080, 2400, 416, 416, 411, 914, 16},
    {L"phone-galaxy-s24-ultra", L"Samsung Galaxy S24 Ultra", "samsung", "SM-S928B", "samsung", "e3q",
     "Snapdragon 8 Gen 3 for Galaxy", 10724, 1440, 3120, 505, 503, 384, 832, 16},
    {L"phone-galaxy-a55", L"Samsung Galaxy A55 5G", "samsung", "SM-A556B", "samsung", "a55x",
     "Exynos 1480", 7083, 1080, 2340, 390, 391, 411, 891, 16},
    {L"phone-galaxy-a16", L"Samsung Galaxy A16", "samsung", "SM-A165F", "samsung", "a16",
     "MediaTek Helio G99", 3473, 1080, 2340, 385, 386, 411, 891, 16},
    {L"phone-pixel-8-pro", L"Google Pixel 8 Pro", "Google", "Pixel 8 Pro", "google", "husky",
     "Google Tensor G3", 11744, 1008, 2244, 489, 489, 384, 854, 16},
    {L"phone-pixel-9", L"Google Pixel 9", "Google", "Pixel 9", "google", "tokay", "Google Tensor G4",
     11746, 1080, 2424, 422, 422, 411, 923, 16},
    {L"phone-redmi-note-13-pro", L"Xiaomi Redmi Note 13 Pro", "Xiaomi", "23117RA68G", "Redmi", "garnet",
     "Snapdragon 7s Gen 2", 7454, 1220, 2712, 446, 446, 443, 986, 15},
    {L"phone-oneplus-12", L"OnePlus 12", "OnePlus", "CPH2581", "OnePlus", "waffle", "Snapdragon 8 Gen 3",
     11486, 1440, 3168, 510, 510, 411, 905, 16},
    {L"phone-moto-g84", L"Motorola moto g84 5G", "motorola", "moto g84 5G", "motorola", "bangkk",
     "Snapdragon 695 5G", 11268, 1080, 2400, 402, 402, 411, 914, 15},
};

const Phone* find_phone(const std::wstring& id) {
  for (const Phone& phone : kPhones)
    if (id == phone.id) return &phone;
  return nullptr;
}

std::string phone_user_agent(const Phone& p) {
  std::string label = p.manufacturer;
  if (!label.empty()) label[0] = static_cast<char>(toupper(static_cast<unsigned char>(label[0])));
  label += std::string(" ") + p.model;
  char ua[512];
  std::snprintf(ua, sizeof ua,
                "Mozilla/5.0 (%dMB; %dx%d; %dx%d; %dx%d; %s; %d) AppleWebKit/537.36 (KHTML, like Gecko)  ROBLOX Android App "
                "{roblox_version} Phone Hybrid()  GooglePlayStore RobloxApp/{roblox_version} (GlobalDist; GooglePlayStore)",
                p.ram, p.width, p.height, p.xdpi, p.ydpi, p.dp_width, p.dp_height, label.c_str(), p.android);
  return ua;
}

std::string windows_name() {
  const OsInfo os = os_info();
  if (os.windows7()) return "Windows 7";
  if (os.major == 6) return "Windows 8";
  return os.build >= 22000 ? "Windows 11" : "Windows 10";
}

std::wstring ini_path(const Paths& paths) { return paths.home + L"\\launcher.ini"; }
std::wstring config_path(const Paths& paths) { return paths.data + L"\\root\\.config\\blox7\\config.yaml"; }
std::wstring fflags_path(const Paths& paths) { return paths.data + L"\\root\\.config\\blox7\\fflags.json"; }
std::wstring window_state_path(const Paths& paths) {
  return paths.data + L"\\root\\.local\\state\\blox7\\window-state.json";
}

std::map<std::string, std::string> read_ini(const std::wstring& path) {
  std::map<std::string, std::string> values;
  for (const std::string& line : split_lines(read_all(path))) {
    const std::string t = trimmed(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') continue;
    const size_t eq = t.find('=');
    if (eq != std::string::npos) values[trimmed(t.substr(0, eq))] = trimmed(t.substr(eq + 1));
  }
  return values;
}

bool write_ini(const std::wstring& path, const std::map<std::string, std::string>& values) {
  std::string text;
  for (const auto& [key, value] : values)
    if (!value.empty()) text += key + "=" + value + "\n";
  make_parent(path);
  return write_all(path, text);
}

// hand-rolled json so untouched flags come back byte for byte
std::vector<std::pair<std::string, std::string>> read_flags(const std::string& json) {
  std::vector<std::pair<std::string, std::string>> flags;
  size_t i = json.find('{');
  if (i == std::string::npos) return flags;
  ++i;
  while (i < json.size()) {
    const size_t key_open = json.find('"', i);
    if (key_open == std::string::npos) break;
    const size_t key_close = json.find('"', key_open + 1);
    const size_t colon = key_close == std::string::npos ? key_close : json.find(':', key_close);
    if (colon == std::string::npos) break;
    size_t v = json.find_first_not_of(" \t\r\n", colon + 1);
    if (v == std::string::npos) break;
    size_t end;
    if (json[v] == '"') {
      end = v + 1;
      while (end < json.size() && json[end] != '"') end += json[end] == '\\' ? 2 : 1;
      ++end;
    } else {
      end = json.find_first_of(",}\r\n", v);
      if (end == std::string::npos) end = json.size();
    }
    flags.emplace_back(json.substr(key_open + 1, key_close - key_open - 1), trimmed(json.substr(v, end - v)));
    i = json.find_first_of(",}", end);
    if (i == std::string::npos || json[i] == '}') break;
    ++i;
  }
  return flags;
}

std::string write_flags(const std::vector<std::pair<std::string, std::string>>& flags) {
  std::string json = "{\n";
  for (size_t i = 0; i < flags.size(); ++i)
    json += "  \"" + flags[i].first + "\": " + flags[i].second + (i + 1 < flags.size() ? ",\n" : "\n");
  return json + "}\n";
}

std::string set_xml_value(std::string xml, const char* name, const std::string& value) {
  const std::string needle = std::string("name=\"") + name + "\">";
  const size_t at = xml.find(needle);
  if (at == std::string::npos) return xml;
  const size_t start = at + needle.size();
  const size_t end = xml.find('<', start);
  if (end == std::string::npos) return xml;
  return xml.replace(start, end - start, value);
}

std::wstring basic_settings_path(const Paths& paths) {
  const std::wstring dir = paths.data + L"\\root\\.local\\share\\blox7\\android\\data\\files\\appData";
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW((dir + L"\\GlobalBasicSettings_*.xml").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return {};
  std::wstring name = found.cFileName;
  FindClose(search);
  return dir + L"\\" + name;
}

struct Preset {
  const char* id;
  int saved_level;
  int graphics_level;
  bool max_quality;
  bool flags;
};

const Preset kPresets[] = {
    {"performance", 1, 1, false, false},
    {"balanced", 5, 10, false, false},
    {"high", 8, 17, false, false},
    {"superhq", 10, 21, true, true},
};

const char* const kPresetFlags[][2] = {
    {"FIntDebugForceMSAASamples", "4"},
    {"DFIntTextureQualityOverride", "3"},
    {"DFFlagTextureQualityOverrideEnabled", "\"True\""},
};

void apply_quality(const Paths& paths, const Settings& s) {
  const std::string id = narrow(s.quality);
  const Preset* preset = nullptr;
  for (const Preset& p : kPresets)
    if (id == p.id) preset = &p;

  auto ini = read_ini(ini_path(paths));
  const bool owned = ini["preset_flags"] == "1";
  const std::wstring flags_file = fflags_path(paths);
  auto flags = read_flags(read_all(flags_file));
  const auto drop_owned = [&] {
    for (const auto& pair : kPresetFlags)
      flags.erase(std::remove_if(flags.begin(), flags.end(), [&](const auto& f) { return f.first == pair[0]; }),
                  flags.end());
  };
  if (preset && preset->flags) {
    drop_owned();
    for (const auto& pair : kPresetFlags) flags.emplace_back(pair[0], pair[1]);
    ini["preset_flags"] = "1";
  } else if (owned) {
    drop_owned();
    ini["preset_flags"] = "";
  }
  if ((preset && preset->flags) || owned) {
    make_parent(flags_file);
    write_all(flags_file, write_flags(flags));
    write_ini(ini_path(paths), ini);
  }

  if (!preset) return;
  const std::wstring xml_path = basic_settings_path(paths);
  if (xml_path.empty()) return;
  std::string xml = read_all(xml_path);
  if (xml.empty()) return;
  xml = set_xml_value(xml, "SavedQualityLevel", std::to_string(preset->saved_level));
  xml = set_xml_value(xml, "GraphicsQualityLevel", std::to_string(preset->graphics_level));
  xml = set_xml_value(xml, "MaxQualityEnabled", preset->max_quality ? "true" : "false");
  write_all(xml_path, xml);
}

enum class FlagKind { kNone, kFlag, kInt, kString, kLog };

FlagKind flag_kind(const std::wstring& name) {
  const size_t at = name.size() > 1 && (name[0] == L'D' || name[0] == L'S') && name[1] == L'F' ? 1 : 0;
  if (name.size() <= at || name[at] != L'F') return FlagKind::kNone;
  for (const wchar_t ch : name)
    if (ch > 127 || !(iswalnum(ch) || ch == L'_')) return FlagKind::kNone;
  const struct {
    const wchar_t* word;
    FlagKind kind;
  } kinds[] = {{L"Flag", FlagKind::kFlag}, {L"Int", FlagKind::kInt}, {L"String", FlagKind::kString}, {L"Log", FlagKind::kLog}};
  for (const auto& kind : kinds) {
    const size_t length = wcslen(kind.word);
    if (name.compare(at + 1, length, kind.word) == 0 && name.size() > at + 1 + length) return kind.kind;
  }
  return FlagKind::kNone;
}

std::string json_string(const std::wstring& value) {
  std::string out = "\"";
  for (const char ch : narrow(value)) {
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += ch;
    } else {
      out += ch;
    }
  }
  return out + "\"";
}

std::wstring shown_value(const std::string& raw) {
  if (raw.size() < 2 || raw.front() != '"') return widen(raw);
  std::string out;
  for (size_t i = 1; i + 1 < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 2 < raw.size()) ++i;
    out += raw[i];
  }
  return widen(out);
}

bool is_preset_flag(const std::string& name) {
  for (const auto& pair : kPresetFlags)
    if (name == pair[0]) return true;
  return false;
}

std::wstring trimmed_wide(const std::wstring& s) {
  const size_t start = s.find_first_not_of(L" \t");
  if (start == std::wstring::npos) return {};
  return s.substr(start, s.find_last_not_of(L" \t") - start + 1);
}

std::wstring short_gpu_name(std::wstring name) {
  for (const wchar_t* junk : {L"(R)", L"(r)", L"(TM)", L"(tm)"})
    for (size_t at; (at = name.find(junk)) != std::wstring::npos;) name.erase(at, wcslen(junk));
  for (const wchar_t* vendor : {L"NVIDIA GeForce ", L"NVIDIA ", L"AMD "})
    if (name.rfind(vendor, 0) == 0) {
      name.erase(0, wcslen(vendor));
      break;
    }
  for (size_t at; (at = name.find(L"  ")) != std::wstring::npos;) name.erase(at, 1);
  while (!name.empty() && name.back() == L' ') name.pop_back();
  return name;
}

int windows_gpu_preference(const std::wstring& id) {
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
  const auto create = dxgi ? reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
  IDXGIFactory6* factory = nullptr;
  if (!create || FAILED(create(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory)))) return 0;
  int preference = 0;
  for (const auto& [order, value] : {std::pair{DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, 2}, std::pair{DXGI_GPU_PREFERENCE_MINIMUM_POWER, 1}}) {
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(factory->EnumAdapterByGpuPreference(0, order, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter))))
      continue;
    DXGI_ADAPTER_DESC1 desc{};
    wchar_t found[16] = L"";
    if (SUCCEEDED(adapter->GetDesc1(&desc))) std::swprintf(found, 16, L"%04x:%04x", desc.VendorId, desc.DeviceId);
    adapter->Release();
    if (id == found) {
      preference = value;
      break;
    }
  }
  factory->Release();
  return preference;
}

void set_windows_gpu_preference(const Paths& paths, int preference, std::map<std::string, std::string>& ini) {
  const std::wstring written = widen(ini["gpu_preference"]);
  if (!preference && written.empty()) return;
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr) != ERROR_SUCCESS)
    return;
  if (!written.empty() && (!preference || written != paths.exe)) {
    RegDeleteValueW(key, written.c_str());
    ini["gpu_preference"] = "";
  }
  if (preference) {
    const std::wstring value = L"GpuPreference=" + std::to_wstring(preference) + L";";
    if (RegSetValueExW(key, paths.exe.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS)
      ini["gpu_preference"] = narrow(paths.exe);
  }
  RegCloseKey(key);
}

// the join button on roblox.com. HKCU so no admin, and whoever had the links before gets parked till we let go
constexpr const wchar_t* kLinkSchemes[] = {L"roblox", L"roblox-player"};

std::wstring link_key(const wchar_t* scheme) { return std::wstring(L"Software\\Classes\\") + scheme; }
std::wstring parked_key(const wchar_t* scheme) { return std::wstring(L"Software\\Blox7\\Replaced links\\") + scheme; }
std::wstring link_command(const std::wstring& exe) { return L"\"" + exe + L"\" \"%1\""; }

bool key_exists(const std::wstring& path) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
  RegCloseKey(key);
  return true;
}

bool links_to(const wchar_t* scheme, const std::wstring& exe) {
  wchar_t command[2048];
  DWORD size = sizeof command;
  return !exe.empty() &&
         RegGetValueW(HKEY_CURRENT_USER, (link_key(scheme) + L"\\shell\\open\\command").c_str(), nullptr, RRF_RT_REG_SZ,
                      nullptr, command, &size) == ERROR_SUCCESS &&
         _wcsicmp(command, link_command(exe).c_str()) == 0;
}

LSTATUS copy_key(const std::wstring& from, const std::wstring& to) {
  HKEY source = nullptr, target = nullptr;
  LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, from.c_str(), 0, KEY_READ, &source);
  if (status == ERROR_SUCCESS)
    status = RegCreateKeyExW(HKEY_CURRENT_USER, to.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &target, nullptr);
  if (status == ERROR_SUCCESS) status = RegCopyTreeW(source, nullptr, target);
  if (target) RegCloseKey(target);
  if (source) RegCloseKey(source);
  if (status != ERROR_SUCCESS) RegDeleteTreeW(HKEY_CURRENT_USER, to.c_str());
  return status;
}

LSTATUS take_link(const wchar_t* scheme, const std::wstring& exe, bool ours) {
  const std::wstring key = link_key(scheme), parked = parked_key(scheme);
  const bool parking = !ours && key_exists(key);
  if (!ours) RegDeleteTreeW(HKEY_CURRENT_USER, parked.c_str());
  if (parking) {
    const LSTATUS status = copy_key(key, parked);
    if (status != ERROR_SUCCESS) return status;
  }
  RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
  const auto set = [&key](const wchar_t* sub, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, (key + sub).c_str(), name, REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  };
  LSTATUS status = set(L"\\shell\\open\\command", nullptr, link_command(exe));
  if (status == ERROR_SUCCESS) status = set(L"\\DefaultIcon", nullptr, exe + L",0");
  if (status == ERROR_SUCCESS) status = set(L"", nullptr, L"URL: Roblox Protocol");
  if (status == ERROR_SUCCESS) status = set(L"", L"URL Protocol", L"");
  if (status != ERROR_SUCCESS) {
    RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    if (parking && copy_key(parked, key) == ERROR_SUCCESS) RegDeleteTreeW(HKEY_CURRENT_USER, parked.c_str());
  }
  return status;
}

void give_back_link(const wchar_t* scheme, bool ours) {
  const std::wstring key = link_key(scheme), parked = parked_key(scheme);
  if (ours) {
    RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    if (key_exists(parked) && copy_key(parked, key) != ERROR_SUCCESS) return;
  }
  RegDeleteTreeW(HKEY_CURRENT_USER, parked.c_str());
}

void drop_if_empty(const wchar_t* path) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
  DWORD subkeys = 1, values = 1;
  RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
  RegCloseKey(key);
  if (!subkeys && !values) RegDeleteKeyW(HKEY_CURRENT_USER, path);
}

std::wstring set_join_links(const Paths& paths, bool on, std::map<std::string, std::string>& ini) {
  const std::wstring linked = widen(ini["join_links"]);
  std::wstring error;
  for (const wchar_t* scheme : kLinkSchemes) {
    const bool current = links_to(scheme, paths.exe), ours = current || links_to(scheme, linked);
    if (!on) {
      if (ours || key_exists(parked_key(scheme))) give_back_link(scheme, ours);
    } else if (!current) {
      const LSTATUS status = take_link(scheme, paths.exe, ours);
      if (status != ERROR_SUCCESS && error.empty())
        error = L"cannot register the " + std::wstring(scheme) + L" links (error " + std::to_wstring(status) + L")";
    }
  }
  drop_if_empty(L"Software\\Blox7\\Replaced links");
  drop_if_empty(L"Software\\Blox7");
  ini["join_links"] = on ? narrow(paths.exe) : "";
  return error;
}

}  // namespace

Settings load_settings(const Paths& paths) {
  Settings s;
  const std::string config = read_all(config_path(paths));
  const auto pick = [&](const char* section, const char* key, const std::wstring& fallback) {
    const std::string value = yaml_get(config, section, key);
    return value.empty() ? fallback : widen(value);
  };
  const auto flag = [&](const char* section, const char* key, bool fallback) {
    const std::string value = yaml_get(config, section, key);
    if (value == "true" || value == "yes" || value == "on" || value == "1") return true;
    if (value == "false" || value == "no" || value == "off" || value == "0") return false;
    return fallback;
  };
  s.backend = pick("graphics", "backend", s.backend);
  s.frame_rate_limit = pick("graphics", "frame_rate_limit", s.frame_rate_limit);
  s.vsync = pick("graphics", "vsync", s.vsync);
  s.present_mode = pick("graphics", "present_mode", s.present_mode);
  s.worker_mode = pick("performance", "physics_worker_mode", s.worker_mode);
  s.threaded_rendering = flag("performance", "multithreaded_rendering", s.threaded_rendering);
  s.high_dpi = flag("window", "high_dpi", s.high_dpi);
  const int width = _wtoi(pick("window", "width", L"0").c_str());
  const int height = _wtoi(pick("window", "height", L"0").c_str());
  if (width > 0 && height > 0) {
    s.width = width;
    s.height = height;
  }
  const auto ini = read_ini(ini_path(paths));
  if (const auto device = yaml_top(config, "device")) {
    s.device = widen(*device);
    const auto profile = ini.find("profile");
    if (device->empty()) s.device = profile != ini.end() ? widen(profile->second) : std::wstring();
  }

  const std::string state = read_all(window_state_path(paths));
  const size_t full = state.find("\"fullscreen\"");
  s.fullscreen = full != std::string::npos && state.find("true", full) < state.find_first_of(",}", full);

  if (const auto quality = ini.find("quality"); quality != ini.end()) s.quality = widen(quality->second);
  if (const auto gpu = ini.find("gpu"); gpu != ini.end()) s.gpu = widen(gpu->second);
  if (const auto links = ini.find("join_links"); links != ini.end()) s.join_links = !links->second.empty();
  if (const auto refresh = ini.find("refresh_rate"); refresh != ini.end()) s.refresh_rate = std::max(0, atoi(refresh->second.c_str()));
  const auto number = [&ini](const char* key, float fallback) {
    const auto found = ini.find(key);
    return found == ini.end() ? fallback : static_cast<float>(atof(found->second.c_str()));
  };
  if (const auto effects = ini.find("effects"); effects != ini.end()) s.effects = widen(effects->second);
  s.sharpen = std::clamp(number("fx_sharpen", 0), 0.0f, 1.0f);
  s.vibrance = std::clamp(number("fx_vibrance", 0), -1.0f, 1.0f);
  s.contrast = std::clamp(number("fx_contrast", 1), 0.5f, 1.5f);
  s.brightness = std::clamp(number("fx_brightness", 0), -0.25f, 0.25f);
  s.fxaa = number("fx_fxaa", 0) > 0.5f;
  if (const auto shader = ini.find("shader"); shader != ini.end()) s.shader = widen(shader->second);
  if (s.effects != L"custom") effects_preset(&s, s.effects);
  return s;
}

void effects_preset(Settings* s, const std::wstring& preset) {
  s->effects = preset;
  if (preset == L"custom") return;
  s->sharpen = 0;
  s->vibrance = 0;
  s->contrast = 1;
  s->brightness = 0;
  if (preset == L"crisp") {
    s->sharpen = 0.6f;
  } else if (preset == L"vivid") {
    s->sharpen = 0.4f;
    s->vibrance = 0.35f;
    s->contrast = 1.08f;
  } else if (preset == L"cinematic") {
    s->sharpen = 0.3f;
    s->vibrance = 0.15f;
    s->contrast = 1.15f;
    s->brightness = -0.02f;
  }
}

std::wstring save_settings(const Paths& paths, const Settings& s) {
  const std::wstring path = config_path(paths);
  std::string config = read_all(path);
  if (config.empty()) config = kDefaultConfig;
  config = yaml_set(config, "graphics", "backend", narrow(s.backend));
  config = yaml_set(config, "graphics", "frame_rate_limit", narrow(s.frame_rate_limit));
  config = yaml_set(config, "graphics", "vsync", narrow(s.vsync));
  config = yaml_set(config, "graphics", "present_mode", narrow(s.present_mode));
  config = yaml_set(config, "performance", "physics_worker_mode", narrow(s.worker_mode));
  config = yaml_set(config, "performance", "multithreaded_rendering", s.threaded_rendering ? "true" : "false");
  config = yaml_set(config, "window", "width", std::to_string(s.width));
  config = yaml_set(config, "window", "height", std::to_string(s.height));
  config = yaml_set(config, "window", "high_dpi", s.high_dpi ? "true" : "false");
  std::string profile;
  if (const Phone* phone = find_phone(s.device)) {
    const std::string code = phone->code;
    const std::string sku = std::string(phone->brand) == "samsung" ? code + "xeea" : code;
    config = yaml_set_block(config, "device",
                            {"type: mobile-s22-ultra", "platform: Android", "name: " + narrow(phone->name),
                             std::string("manufacturer: ") + phone->manufacturer, std::string("model: ") + phone->model,
                             std::string("brand: ") + phone->brand, "code: " + code, "sku: " + sku,
                             std::string("soc_model: ") + phone->soc});
    profile = narrow(s.device);
  } else if (s.device == L"pc-windows-11" && windows_name() != "Windows 11") {
    const std::string name = windows_name() + " PC";
    std::string code = windows_name(), sku = windows_name();
    for (char& ch : code) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    code.erase(std::remove(code.begin(), code.end(), ' '), code.end());
    for (char& ch : sku) ch = ch == ' ' ? '-' : static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    config = yaml_set_block(config, "device",
                            {"type: pc-windows-11", "platform: Windows", "name: " + name, "manufacturer: Microsoft",
                             "model: " + name, "brand: Microsoft", "code: " + code, "sku: " + sku + "-x86_64",
                             "soc_model: x86_64"});
    profile = "pc-windows-11";
  } else if (!s.device.empty() && s.device.find(L'\n') == std::wstring::npos) {
    config = yaml_set_top(config, "device", narrow(s.device));
  }
  make_parent(path);
  if (!write_all(path, config)) return L"cannot write " + path;

  const std::wstring state_path = window_state_path(paths);
  std::string state = read_all(state_path);
  const size_t full = state.find("\"fullscreen\"");
  if (full != std::string::npos) {
    const size_t value = state.find_first_not_of(" \t:", full + 12);
    const size_t end = state.find_first_of(",}", value);
    if (value != std::string::npos && end != std::string::npos) {
      state.replace(value, end - value, s.fullscreen ? "true" : "false");
      write_all(state_path, state);
    }
  } else if (state.empty() && s.fullscreen) {
    char json[320];
    std::snprintf(json, sizeof json,
                  "{\"configured\":{\"height\":%d,\"width\":%d},\"fullscreen\":true,\"maximized\":false,"
                  "\"schema_version\":1,\"windowed\":{\"has_position\":false,\"height\":%d,\"width\":%d}}",
                  s.height, s.width, s.height, s.width);
    make_parent(state_path);
    write_all(state_path, json);
  }

  auto ini = read_ini(ini_path(paths));
  ini["quality"] = narrow(s.quality);
  ini["gpu"] = narrow(s.gpu);
  ini["refresh_rate"] = s.refresh_rate > 0 ? std::to_string(s.refresh_rate) : "";
  ini["profile"] = profile;
  const auto decimal = [](float value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", value);
    return std::string(text);
  };
  ini["effects"] = s.effects == L"off" ? "" : narrow(s.effects);
  ini["fx_sharpen"] = s.effects == L"custom" ? decimal(s.sharpen) : "";
  ini["fx_vibrance"] = s.effects == L"custom" ? decimal(s.vibrance) : "";
  ini["fx_contrast"] = s.effects == L"custom" ? decimal(s.contrast) : "";
  ini["fx_brightness"] = s.effects == L"custom" ? decimal(s.brightness) : "";
  ini["fx_fxaa"] = s.fxaa ? "1" : "";
  ini["shader"] = narrow(s.shader);
  const bool vulkan = s.backend == L"direct-vulkan" || s.backend == L"vulkan" || s.backend == L"native-vulkan";
  set_windows_gpu_preference(paths, !vulkan && !s.gpu.empty() ? windows_gpu_preference(s.gpu) : 0, ini);
  const std::wstring links = set_join_links(paths, s.join_links, ini);
  if (!write_ini(ini_path(paths), ini)) return L"cannot write launcher.ini";
  return links;
}

void refresh_join_links(const Paths& paths) {
  auto ini = read_ini(ini_path(paths));
  const std::wstring linked = widen(ini["join_links"]);
  if (linked.empty()) return;
  bool current = _wcsicmp(linked.c_str(), paths.exe.c_str()) == 0;
  for (const wchar_t* scheme : kLinkSchemes) current = current && links_to(scheme, paths.exe);
  if (current) return;
  set_join_links(paths, true, ini);
  write_ini(ini_path(paths), ini);
}

void apply_launch_settings(const Paths& paths) {
  const Settings s = load_settings(paths);
  apply_quality(paths, s);
  SetEnvironmentVariableW(L"BLOX7_VULKAN_DEVICE", s.gpu.empty() ? nullptr : s.gpu.c_str());
  if (s.quality == L"high" || s.quality == L"superhq") SetEnvironmentVariableW(L"BLOX7_GRAPHICS_QUALITY", L"high");
  else if (s.quality == L"performance") SetEnvironmentVariableW(L"BLOX7_GRAPHICS_QUALITY", L"1");
  else SetEnvironmentVariableW(L"BLOX7_GRAPHICS_QUALITY", nullptr);
  SetEnvironmentVariableW(L"BLOX7_FULLSCREEN_REFRESH",
                          s.refresh_rate > 0 ? std::to_wstring(s.refresh_rate).c_str() : nullptr);
  const Phone* phone = find_phone(s.device);
  SetEnvironmentVariableW(L"BLOX7_USER_AGENT", phone ? widen(phone_user_agent(*phone)).c_str() : nullptr);

  const bool vulkan = s.backend == L"direct-vulkan" || s.backend == L"vulkan" || s.backend == L"native-vulkan";
  Settings fx = s;
  if (fx.effects != L"custom") effects_preset(&fx, fx.effects);
  const bool tuned = fx.sharpen > 0 || fx.vibrance != 0 || fx.contrast != 1 || fx.brightness != 0 || fx.fxaa;
  if (vulkan && (tuned || !fx.shader.empty())) {
    wchar_t value[160];
    std::swprintf(value, 160, L"sharpen=%.2f,vibrance=%.2f,contrast=%.2f,brightness=%.2f,fxaa=%d", fx.sharpen, fx.vibrance,
                  fx.contrast, fx.brightness, fx.fxaa ? 1 : 0);
    SetEnvironmentVariableW(L"BLOX7_POST_FX", value);
  } else {
    SetEnvironmentVariableW(L"BLOX7_POST_FX", nullptr);
  }
  std::wstring compiled;
  if (vulkan && !fx.shader.empty()) {
    bool rebuilt = false;
    if (refresh_shader(paths, fx.shader, &rebuilt).empty()) compiled = widen(posix_path(paths.home + L"\\shader.spv"));
  }
  SetEnvironmentVariableW(L"BLOX7_POST_SHADER", compiled.empty() ? nullptr : compiled.c_str());
}

std::wstring refresh_shader(const Paths& paths, const std::wstring& source, bool* rebuilt) {
  *rebuilt = false;
  const std::wstring spv = paths.home + L"\\shader.spv";
  WIN32_FILE_ATTRIBUTE_DATA from{}, built{};
  if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &from)) return L"The shader file is gone.";
  if (GetFileAttributesExW(spv.c_str(), GetFileExInfoStandard, &built) &&
      CompareFileTime(&built.ftLastWriteTime, &from.ftLastWriteTime) >= 0)
    return {};
  const std::wstring error = compile_shader(paths, source);
  *rebuilt = error.empty();
  return error;
}

std::wstring compile_shader(const Paths& paths, const std::wstring& source) {
  const std::wstring compiler = paths.runtime + L"\\bin\\glslc.exe";
  if (GetFileAttributesW(compiler.c_str()) == INVALID_FILE_ATTRIBUTES) return L"glslc.exe is missing from the Blox7 runtime.";
  if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) return L"The shader file is gone.";
  const std::wstring spv = paths.home + L"\\shader.spv", fresh = spv + L".new";
  std::wstring command = L"\"" + compiler + L"\" -O --target-env=vulkan1.0 -fshader-stage=frag \"" + source + L"\" -o \"" + fresh + L"\"";
  SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
  HANDLE reader = nullptr, writer = nullptr;
  if (!CreatePipe(&reader, &writer, &inherit, 0)) return L"Blox7 couldn't start glslc.";
  SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = writer;
  startup.hStdError = writer;
  PROCESS_INFORMATION process{};
  const BOOL started =
      CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(writer);
  if (!started) {
    CloseHandle(reader);
    return L"Blox7 couldn't start glslc.";
  }
  std::string output;
  char buffer[4096];
  DWORD got = 0;
  while (ReadFile(reader, buffer, sizeof buffer, &got, nullptr) && got) output.append(buffer, got);
  CloseHandle(reader);
  WaitForSingleObject(process.hProcess, 30000);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  if (code == 0)
    return MoveFileExW(fresh.c_str(), spv.c_str(), MOVEFILE_REPLACE_EXISTING) ? std::wstring()
                                                                              : L"Blox7 couldn't save the compiled shader.";
  DeleteFileW(fresh.c_str());
  if (code == 0xC0000135) return L"glslc needs the Universal C Runtime (Windows Update KB2999226).";
  std::string first;
  for (const std::string& line : split_lines(output))
    if (line.find("error") != std::string::npos) {
      first = line;
      break;
    }
  if (first.empty()) first = output.empty() ? "glslc failed." : split_lines(output).front();
  const std::string file = narrow(source);
  if (first.rfind(file + ":", 0) == 0) first = "line " + first.substr(file.size() + 1);
  return widen(first);
}

std::vector<int> refresh_rates() {
  std::vector<int> rates;
  DEVMODEW current{};
  current.dmSize = sizeof current;
  if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current)) return rates;
  DEVMODEW mode{};
  mode.dmSize = sizeof mode;
  for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); ++i) {
    if (mode.dmPelsWidth != current.dmPelsWidth || mode.dmPelsHeight != current.dmPelsHeight || mode.dmDisplayFrequency <= 1)
      continue;
    const int hz = static_cast<int>(mode.dmDisplayFrequency);
    if (std::find(rates.begin(), rates.end(), hz) == rates.end()) rates.push_back(hz);
  }
  std::sort(rates.begin(), rates.end());
  return rates;
}

std::vector<FastFlag> load_fast_flags(const Paths& paths) {
  const bool owned = read_ini(ini_path(paths))["preset_flags"] == "1";
  std::vector<FastFlag> flags;
  for (const auto& [name, raw] : read_flags(read_all(fflags_path(paths))))
    flags.push_back({widen(name), shown_value(raw), owned && is_preset_flag(name)});
  return flags;
}

std::wstring check_fast_flag(const std::wstring& name, std::wstring* value, bool super_hq) {
  const FlagKind kind = flag_kind(name);
  if (kind == FlagKind::kNone) return L"Flag names start with FFlag, FInt, FString or FLog.";
  const std::string plain = narrow(name);
  for (const char* forced : kForcedFlags)
    if (plain == forced) return L"Blox7 sets this flag itself, so it can't be added.";
  if (super_hq && is_preset_flag(plain)) return L"Super HQ sets this flag. Change the preset first.";
  std::wstring v = trimmed_wide(*value);
  if (kind == FlagKind::kFlag) {
    std::wstring lower = v;
    for (wchar_t& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
    if (lower == L"true" || lower == L"1" || lower == L"yes" || lower == L"on") v = L"True";
    else if (lower == L"false" || lower == L"0" || lower == L"no" || lower == L"off") v = L"False";
    else return L"This flag takes True or False.";
  } else if (kind == FlagKind::kInt || kind == FlagKind::kLog) {
    const size_t digits = v.size() - (!v.empty() && v[0] == L'-' ? 1 : 0);
    if (digits == 0 || digits > 18 || v.find_first_not_of(L"0123456789", v[0] == L'-' ? 1 : 0) != std::wstring::npos)
      return L"This flag takes a whole number.";
  } else {
    for (const wchar_t ch : v)
      if (ch < 0x20) return L"The value can't contain tabs or line breaks.";
  }
  *value = v;
  return {};
}

std::wstring save_fast_flags(const Paths& paths, const std::vector<FastFlag>& flags) {
  const std::wstring file = fflags_path(paths);
  const auto current = read_flags(read_all(file));
  std::vector<std::pair<std::string, std::string>> out;
  for (const FastFlag& flag : flags) {
    const std::string name = narrow(flag.name);
    if (flag.managed) {
      for (const auto& entry : current)
        if (entry.first == name) out.push_back(entry);
      continue;
    }
    const FlagKind kind = flag_kind(flag.name);
    out.emplace_back(name, kind == FlagKind::kInt || kind == FlagKind::kLog ? narrow(flag.value) : json_string(flag.value));
  }
  make_parent(file);
  return write_all(file, write_flags(out)) ? std::wstring() : L"cannot write " + file;
}

std::wstring import_fast_flags(const std::wstring& file, std::vector<FastFlag>* flags, bool super_hq, bool* failed) {
  const auto entries = read_flags(read_all(file));
  *failed = entries.empty();
  if (entries.empty()) return L"No flags in that file.";
  int added = 0, changed = 0, skipped = 0;
  for (const auto& [name, raw] : entries) {
    const std::wstring wide_name = widen(name);
    std::wstring value = shown_value(raw);
    if (!check_fast_flag(wide_name, &value, super_hq).empty()) {
      ++skipped;
      continue;
    }
    auto found = std::find_if(flags->begin(), flags->end(), [&](const FastFlag& flag) { return flag.name == wide_name; });
    if (found == flags->end()) {
      flags->push_back({wide_name, value, false});
      ++added;
    } else if (found->managed) {
      ++skipped;
    } else if (found->value != value) {
      found->value = value;
      ++changed;
    }
  }
  std::wstring summary = L"Imported " + std::to_wstring(added) + L" new, " + std::to_wstring(changed) + L" changed";
  if (skipped) summary += L", skipped " + std::to_wstring(skipped);
  return summary + L".";
}

std::wstring export_fast_flags(const std::wstring& file, const std::vector<FastFlag>& flags) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const FastFlag& flag : flags) {
    if (flag.managed) continue;
    const FlagKind kind = flag_kind(flag.name);
    out.emplace_back(narrow(flag.name),
                     kind == FlagKind::kInt || kind == FlagKind::kLog ? narrow(flag.value) : json_string(flag.value));
  }
  return write_all(file, write_flags(out)) ? std::wstring() : L"Can't write " + file + L".";
}

std::vector<DeviceChoice> device_choices() {
  std::vector<DeviceChoice> choices{
      {L"mobile-s22-ultra", L"Samsung Galaxy S22 Ultra", L"Exynos 2200", DeviceKind::kPhone}};
  for (const Phone& phone : kPhones) {
    std::wstring soc = widen(phone.soc);
    if (const size_t cut = soc.find(L" for "); cut != std::wstring::npos) soc.erase(cut);
    choices.push_back({phone.id, phone.name, soc, DeviceKind::kPhone});
  }
  choices.push_back({L"pc-windows-11", widen(windows_name()) + L" PC", L"Mouse and keyboard", DeviceKind::kPc});
  return choices;
}

std::vector<GpuChoice> gpu_choices() {
  std::vector<GpuChoice> gpus;
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
  const auto create = dxgi ? reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
  IDXGIFactory1* factory = nullptr;
  if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return gpus;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId != 0x1414) {
      wchar_t id[16];
      std::swprintf(id, 16, L"%04x:%04x", desc.VendorId, desc.DeviceId);
      if (std::none_of(gpus.begin(), gpus.end(), [&](const GpuChoice& gpu) { return gpu.id == id; }))
        gpus.push_back({id, desc.Description, short_gpu_name(desc.Description), desc.DedicatedVideoMemory});
    }
    adapter->Release();
  }
  factory->Release();
  return gpus;
}

}  // namespace blox7
