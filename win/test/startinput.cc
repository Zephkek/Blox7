#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "runtime/roblox_start_input.h"

namespace {

using blox7::runtime::FindTouchStart;
using blox7::runtime::IsTouchStart;
using blox7::runtime::StartGamesOnMouse;

const std::uint8_t kSite748[] = {
    0x48, 0x89, 0xdf, 0xe8, 0xd9, 0x06, 0x00, 0x00, 0x84, 0xc0, 0x74, 0x3d,
    0xc7, 0x83, 0x20, 0x03, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x80, 0xbb,
    0xda, 0x00, 0x00, 0x00, 0x00, 0x74, 0x3d, 0xc6, 0x83, 0xda, 0x00, 0x00,
    0x00, 0x00, 0x48, 0x8d, 0x05, 0x72, 0x91, 0x92, 0x04, 0x48, 0x8d, 0x75,
    0xc0, 0x48, 0x89, 0x06, 0x48, 0x8d, 0x05, 0xb4, 0xf0, 0x04, 0x05, 0x48,
    0x89, 0x46, 0x08, 0x48, 0x89, 0xdf, 0xe8, 0x02, 0xea, 0xe7, 0xff, 0xeb,
    0x13, 0x83, 0x3d, 0x4f, 0x48, 0x03, 0x05, 0x04, 0x75, 0x19, 0xc7, 0x83,
    0x20, 0x03, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x48, 0x89, 0xdf, 0xbe,
    0x01, 0x00, 0x00, 0x00, 0xe8, 0x04, 0x6d, 0x03, 0x00, 0xeb, 0x4c, 0xc7,
    0x83, 0x20, 0x03, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
};
constexpr std::size_t kTouch748 = 0x6b;

const std::uint8_t kSite702[] = {
    0x48, 0x89, 0xdf, 0xe8, 0xb2, 0x1d, 0xd6, 0xfd, 0x84, 0xc0, 0x74, 0x38,
    0xc7, 0x83, 0x28, 0x03, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x80, 0xbb,
    0x02, 0x01, 0x00, 0x00, 0x00, 0x74, 0x16, 0xc6, 0x83, 0x02, 0x01, 0x00,
    0x00, 0x00, 0x48, 0x8d, 0x35, 0x2f, 0x9e, 0xa5, 0x02, 0x48, 0x89, 0xdf,
    0xe8, 0x29, 0xa6, 0xbc, 0xfd, 0x48, 0x89, 0xdf, 0xbe, 0x01, 0x00, 0x00,
    0x00, 0xe8, 0x56, 0x06, 0x00, 0x00, 0xeb, 0x59, 0xc7, 0x83, 0x28, 0x03,
    0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
};
constexpr std::size_t kTouch702 = 0x44;

constexpr std::size_t kSize = 0x10000;
constexpr std::size_t kAt = 0x1000;

int g_failures = 0;

void Expect(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++g_failures;
}

std::string Hex(std::uintptr_t value) {
  char text[32];
  std::snprintf(text, sizeof text, "%zx", static_cast<std::size_t>(value));
  return text;
}

std::vector<std::uint8_t> Noise(std::size_t size) {
  std::vector<std::uint8_t> bytes(size);
  std::uint64_t state = 0x9e3779b97f4a7c15ull;
  for (auto& byte : bytes) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    byte = static_cast<std::uint8_t>(state >> 56);
  }
  return bytes;
}

template <std::size_t N>
std::vector<std::uint8_t> WithSite(const std::uint8_t (&site)[N], std::size_t at) {
  std::vector<std::uint8_t> bytes = Noise(kSize);
  std::memcpy(bytes.data() + at, site, N);
  return bytes;
}

std::optional<std::size_t> Find(const std::vector<std::uint8_t>& bytes) {
  return FindTouchStart(bytes.data(), bytes.size());
}

void Mutated(std::size_t offset, std::uint8_t value, bool found, const char* what) {
  std::vector<std::uint8_t> bytes = WithSite(kSite748, kAt);
  bytes[kAt + offset] = value;
  const auto at = Find(bytes);
  Expect(found ? at && *at == kAt + kTouch748 : !at, what);
}

void RealBytes() {
  const std::vector<std::uint8_t> bytes = WithSite(kSite748, kAt);
  const auto found = Find(bytes);
  Expect(found && *found == kAt + kTouch748, "2.748 bytes: finds the touch start in noise");
  Expect(IsTouchStart(bytes.data(), bytes.size(), kAt + kTouch748), "2.748 bytes: accepts it at its offset");
  Expect(!IsTouchStart(bytes.data(), bytes.size(), kAt + kTouch748 + 1), "2.748 bytes: not one byte later");
  Expect(!IsTouchStart(bytes.data(), bytes.size(), kAt + kTouch748 - 1), "2.748 bytes: not one byte earlier");
  Expect(!IsTouchStart(bytes.data(), bytes.size(), kAt + 0x52), "2.748 bytes: not at the gamepad store");
  Expect(!IsTouchStart(bytes.data(), bytes.size(), bytes.size() - 9), "not nine bytes from the end");
  Expect(!IsTouchStart(bytes.data(), bytes.size(), SIZE_MAX), "not at SIZE_MAX");
  Expect(!Find(Noise(kSize)), "nothing in plain noise");

  const auto old = Find(WithSite(kSite702, kAt));
  Expect(old && *old == kAt + kTouch702, "2.702 bytes (no tv check): finds the touch start");
  std::vector<std::uint8_t> old_off = WithSite(kSite702, kAt);
  old_off[kAt + 0x0b] = 0x37;
  Expect(!Find(old_off), "2.702 bytes: branch landing one byte early");

  std::vector<std::uint8_t> twice = WithSite(kSite748, kAt);
  std::memcpy(twice.data() + 0x8000, kSite748, sizeof kSite748);
  Expect(!Find(twice), "two touch starts are not trusted");
  std::vector<std::uint8_t> mixed = WithSite(kSite748, kAt);
  std::memcpy(mixed.data() + 0x8000, kSite702, sizeof kSite702);
  Expect(!Find(mixed), "two touch starts of different shapes are not trusted");

  Mutated(0x51, 0x18, false, "branch landing one byte early");
  Mutated(0x50, 0x7c, false, "branch that is not je or jne");
  Mutated(0x54, 0x28, false, "gamepad store to another field");
  Mutated(0x58, 0x0d, false, "gamepad store of another value");
  Mutated(kTouch748 + 6, 0x04, false, "touch start already on mouse");
  Mutated(kTouch748 + 7, 0x01, false, "touch value wider than a byte");
  Mutated(0x50, 0x74, true, "either branch polarity");
  Mutated(0x4f, 0x05, true, "the compare before the branch does not matter");
  Mutated(0x0c, 0x90, true, "the ten-foot store does not matter");

  std::vector<std::uint8_t> sib = WithSite(kSite748, kAt);
  sib[kAt + 0x0d] = sib[kAt + 0x53] = sib[kAt + kTouch748 + 1] = 0x84;
  Expect(!Find(sib), "sib addressing is not read as a field store");
  std::vector<std::uint8_t> rsi = WithSite(kSite748, kAt);
  rsi[kAt + 0x0d] = rsi[kAt + 0x53] = rsi[kAt + kTouch748 + 1] = 0x86;
  const auto in_rsi = Find(rsi);
  Expect(in_rsi && *in_rsi == kAt + kTouch748, "another low base register");

  const std::size_t tail = kSize - sizeof kSite748;
  const auto at_end = Find(WithSite(kSite748, tail));
  Expect(at_end && *at_end == tail + kTouch748, "touch start ending the code");
  const auto at_start = Find(WithSite(kSite748, 0));
  Expect(at_start && *at_start == kTouch748, "touch start near the start of the code");

  const std::vector<std::uint8_t> noise = Noise(0x100);
  bool tiny = true;
  for (std::size_t size : {0, 1, 9, 10, 0x89, 0x8a, 0x100}) {
    tiny = tiny && !FindTouchStart(noise.data(), size);
  }
  Expect(tiny, "tiny code");
}

enum class Base { kRbx, kR14, kR11 };

struct Shape {
  Base base = Base::kRbx;
  bool near_jump = false;
  bool gamepad_taken = false;
  std::size_t gap = 0x10;
  bool touch_on_r11 = false;
  std::uint32_t gamepad_value = 12;
};

void PutStore(std::vector<std::uint8_t>* out, Base base, std::uint32_t value) {
  if (base != Base::kRbx) out->push_back(0x41);
  out->insert(out->end(), {0xc7, static_cast<std::uint8_t>(base == Base::kR14 ? 0x86 : 0x83), 0x20, 0x03, 0x00, 0x00});
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

void PutGap(std::vector<std::uint8_t>* out, std::size_t size) {
  out->insert(out->end(), size, 0x90);
}

std::vector<std::uint8_t> Build(const Shape& shape, std::size_t* touch) {
  const Base touch_base = shape.touch_on_r11 ? Base::kR11 : shape.base;
  std::vector<std::uint8_t> out;
  PutStore(&out, shape.base, 12);
  PutGap(&out, 0x10);
  out.insert(out.end(), {0x83, 0x3d, 0x4f, 0x48, 0x03, 0x05, 0x04});
  const std::uint8_t condition = shape.gamepad_taken ? 0x84 : 0x85;
  const std::size_t jump = out.size();
  if (shape.near_jump) {
    out.insert(out.end(), {0x0f, condition, 0, 0, 0, 0});
  } else {
    out.insert(out.end(), {static_cast<std::uint8_t>(condition - 0x10), 0});
  }
  const std::size_t next = out.size();
  std::size_t target = 0;
  if (shape.gamepad_taken) {
    *touch = out.size();
    PutStore(&out, touch_base, 7);
    PutGap(&out, shape.gap);
    target = out.size();
    PutStore(&out, shape.base, shape.gamepad_value);
  } else {
    PutStore(&out, shape.base, shape.gamepad_value);
    PutGap(&out, shape.gap);
    target = *touch = out.size();
    PutStore(&out, touch_base, 7);
  }
  PutGap(&out, 0x10);
  const auto offset = static_cast<std::int32_t>(target - next);
  if (shape.near_jump) {
    std::memcpy(out.data() + jump + 2, &offset, sizeof offset);
  } else {
    out[jump + 1] = static_cast<std::uint8_t>(offset);
  }
  return out;
}

std::optional<std::size_t> FindShape(const Shape& shape, std::size_t* expected) {
  std::size_t touch = 0;
  const std::vector<std::uint8_t> body = Build(shape, &touch);
  std::vector<std::uint8_t> bytes = Noise(kSize);
  std::memcpy(bytes.data() + kAt, body.data(), body.size());
  *expected = kAt + touch;
  return Find(bytes);
}

void Shapes() {
  for (int bits = 0; bits < 8; ++bits) {
    Shape shape;
    shape.base = bits & 1 ? Base::kR14 : Base::kRbx;
    shape.near_jump = bits & 2;
    shape.gamepad_taken = bits & 4;
    std::size_t expected = 0;
    const auto found = FindShape(shape, &expected);
    Expect(found && *found == expected,
           std::string("shape ") + (shape.base == Base::kR14 ? "r14" : "rbx") + (shape.near_jump ? " rel32" : " rel8") +
               (shape.gamepad_taken ? " je to the gamepad store" : " jne to the touch store"));
  }
  std::size_t expected = 0;
  Shape long_jump;
  long_jump.near_jump = true;
  long_jump.gap = 0xa0;
  const auto long_branch = FindShape(long_jump, &expected);
  Expect(long_branch && *long_branch == expected, "rel32 branch over 0xa0 bytes");
  Shape edge;
  edge.near_jump = true;
  edge.gap = 0xf0;
  const auto at_reach = FindShape(edge, &expected);
  Expect(at_reach && *at_reach == expected, "branch 0x100 bytes before the touch store");
  Shape beyond;
  beyond.near_jump = true;
  beyond.gap = 0xf1;
  Expect(!FindShape(beyond, &expected), "branch 0x101 bytes before the touch store");
  Shape other_base;
  other_base.touch_on_r11 = true;
  Expect(!FindShape(other_base, &expected), "touch store on r11, gamepad store on rbx");
  Shape not_gamepad;
  not_gamepad.gamepad_value = 11;
  Expect(!FindShape(not_gamepad, &expected), "other edge storing another input type");
}

struct Image {
  std::uint8_t* base = nullptr;
  std::size_t size = 0x20000;
  std::size_t code = 0x1000;
  std::size_t touch = 0;
  std::size_t value = 0;

  Image() = default;
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;
  ~Image() {
    if (base != nullptr) munmap(base, size);
  }
};

constexpr std::uint8_t kId[20] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                                  0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13};
const std::string kIdText = "000102030405060708090a0b0c0d0e0f10111213";

enum class Damage { kNone, kMagic, kMachine, kNoNote, kNoteOutside, kHugeNote, kTwoSites };

bool MakeImage(Damage damage, Image* image) {
  void* p = mmap(nullptr, image->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return false;
  image->base = static_cast<std::uint8_t*>(p);
  std::uint8_t* base = image->base;
  auto* header = reinterpret_cast<Elf64_Ehdr*>(base);
  std::memcpy(header->e_ident, ELFMAG, SELFMAG);
  header->e_ident[EI_CLASS] = ELFCLASS64;
  header->e_ident[EI_DATA] = ELFDATA2LSB;
  header->e_ident[EI_VERSION] = EV_CURRENT;
  header->e_type = ET_DYN;
  header->e_machine = damage == Damage::kMachine ? EM_AARCH64 : EM_X86_64;
  header->e_phoff = sizeof(Elf64_Ehdr);
  header->e_phentsize = sizeof(Elf64_Phdr);
  header->e_phnum = damage == Damage::kNoNote ? 1 : 2;
  if (damage == Damage::kMagic) header->e_ident[1] = 'X';
  auto* segments = reinterpret_cast<Elf64_Phdr*>(base + header->e_phoff);
  segments[0].p_type = PT_LOAD;
  segments[0].p_flags = PF_R | PF_X;
  segments[0].p_filesz = segments[0].p_memsz = image->size;
  segments[1].p_type = PT_NOTE;
  segments[1].p_flags = PF_R;
  segments[1].p_vaddr = damage == Damage::kNoteOutside ? image->size - 8 : 0x200;
  segments[1].p_filesz = sizeof(Elf64_Nhdr) + 4 + sizeof kId;
  Elf64_Nhdr note{4, sizeof kId, NT_GNU_BUILD_ID};
  if (damage == Damage::kHugeNote) note.n_namesz = 0xfffffff0u;
  std::memcpy(base + 0x200, &note, sizeof note);
  std::memcpy(base + 0x200 + sizeof note, "GNU", 4);
  std::memcpy(base + 0x200 + sizeof note + 4, kId, sizeof kId);
  const std::vector<std::uint8_t> noise = Noise(image->size - image->code);
  std::memcpy(base + image->code, noise.data(), noise.size());
  std::memcpy(base + image->code + 0x3000, kSite748, sizeof kSite748);
  if (damage == Damage::kTwoSites) std::memcpy(base + image->code + 0x9000, kSite748, sizeof kSite748);
  image->touch = image->code + 0x3000 + kTouch748;
  image->value = image->touch + 6;
  return mprotect(base, image->size, PROT_READ | PROT_EXEC) == 0;
}

std::string ReadText(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}

void WriteText(const std::string& path, const std::string& text) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

void Images(const std::string& dir) {
  const std::string folder = dir + "/images";
  const std::string cache = folder + "/start-input";
  struct Case {
    Damage damage;
    int value;
    bool cached;
    const char* what;
  };
  const Case cases[] = {
      {Damage::kNone, 4, true, "image: patched and cached under its build id"},
      {Damage::kMagic, 7, false, "image without elf magic: left alone"},
      {Damage::kMachine, 7, false, "image for another machine: left alone"},
      {Damage::kNoNote, 4, false, "image without a build id: patched, nothing cached"},
      {Damage::kNoteOutside, 4, false, "build id note outside the loaded image: not read"},
      {Damage::kHugeNote, 4, false, "build id note with a huge name size: not read"},
      {Damage::kTwoSites, 7, true, "image with two touch starts: left alone, cached as none"},
  };
  for (const Case& c : cases) {
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    Image image;
    if (!MakeImage(c.damage, &image)) {
      Expect(false, std::string("map ") + c.what);
      continue;
    }
    StartGamesOnMouse(reinterpret_cast<std::uintptr_t>(image.base), cache);
    const std::string expected = c.damage == Damage::kTwoSites ? "1 " + kIdText + " none\n"
                                                                : "1 " + kIdText + " " + Hex(image.touch) + "\n";
    const bool cache_ok = c.cached ? ReadText(cache) == expected : !std::filesystem::exists(cache);
    Expect(image.base[image.value] == c.value && cache_ok, c.what);
  }
  std::error_code ignored;
  std::filesystem::remove_all(folder, ignored);
}

struct Mapped {
  std::uint8_t* base = nullptr;
  std::size_t size = 0;
  Mapped() = default;
  Mapped(const Mapped&) = delete;
  Mapped& operator=(const Mapped&) = delete;
  ~Mapped() {
    if (base != nullptr) munmap(base, size);
  }
};

bool Map(const std::string& path, int prot, Mapped* out) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st {};
  void* p = fstat(fd, &st) == 0
                ? mmap(nullptr, static_cast<std::size_t>(st.st_size), prot, MAP_PRIVATE, fd, 0)
                : MAP_FAILED;
  close(fd);
  if (p == MAP_FAILED) return false;
  out->base = static_cast<std::uint8_t*>(p);
  out->size = static_cast<std::size_t>(st.st_size);
  return true;
}

const Elf64_Phdr* Text(const std::uint8_t* base) {
  const auto* header = reinterpret_cast<const Elf64_Ehdr*>(base);
  const auto* segments = reinterpret_cast<const Elf64_Phdr*>(base + header->e_phoff);
  for (std::size_t i = 0; i < header->e_phnum; ++i) {
    if (segments[i].p_type == PT_LOAD && (segments[i].p_flags & PF_X) != 0) return &segments[i];
  }
  return nullptr;
}

double Now() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<double>(t.tv_sec) * 1e3 + static_cast<double>(t.tv_nsec) / 1e6;
}

std::uintptr_t RealFile(const std::string& arg) {
  const std::size_t equals = arg.rfind('=');
  const std::string path = equals == std::string::npos ? arg : arg.substr(0, equals);
  Mapped file;
  if (!Map(path, PROT_READ, &file)) {
    Expect(false, "map " + path);
    return 0;
  }
  const Elf64_Phdr* text = Text(file.base);
  if (text == nullptr) {
    Expect(false, "code segment of " + path);
    return 0;
  }
  const double start = Now();
  const auto at = FindTouchStart(file.base + text->p_vaddr, text->p_filesz);
  const double took = Now() - start;
  const std::uintptr_t rva = at ? text->p_vaddr + *at : 0;
  std::printf("      %s: %s, %.1f MB in %.1f ms\n", path.c_str(), at ? ("+0x" + Hex(rva)).c_str() : "none",
              static_cast<double>(text->p_filesz) / 1048576.0, took);
  if (equals != std::string::npos) {
    Expect(rva == std::strtoull(arg.c_str() + equals + 1, nullptr, 16),
           "touch start of " + path + " at +0x" + arg.substr(equals + 1));
  }
  return rva;
}

int FileByte(const std::string& path, std::uintptr_t at) {
  std::ifstream input(path, std::ios::binary);
  input.seekg(static_cast<std::streamoff>(at));
  return input.get();
}

std::string SectionBuildId(const std::uint8_t* file, std::size_t size) {
  const auto* header = reinterpret_cast<const Elf64_Ehdr*>(file);
  if (header->e_shoff == 0 || header->e_shoff + header->e_shnum * sizeof(Elf64_Shdr) > size) return {};
  const auto* sections = reinterpret_cast<const Elf64_Shdr*>(file + header->e_shoff);
  const Elf64_Shdr& names = sections[header->e_shstrndx];
  for (std::size_t i = 0; i < header->e_shnum; ++i) {
    const char* name = reinterpret_cast<const char*>(file + names.sh_offset + sections[i].sh_name);
    if (std::strcmp(name, ".note.gnu.build-id") != 0) continue;
    Elf64_Nhdr note;
    std::memcpy(&note, file + sections[i].sh_offset, sizeof note);
    const std::uint8_t* desc = file + sections[i].sh_offset + sizeof note + ((note.n_namesz + 3) & ~3u);
    std::string id;
    char hex[3];
    for (std::uint32_t k = 0; k < note.n_descsz; ++k) {
      std::snprintf(hex, sizeof hex, "%02x", desc[k]);
      id += hex;
    }
    return id;
  }
  return {};
}

int Start(const std::string& library, const std::string& cache, std::uintptr_t rva, double* took) {
  Mapped image;
  if (!Map(library, PROT_READ | PROT_EXEC, &image)) return -1;
  const double start = Now();
  StartGamesOnMouse(reinterpret_cast<std::uintptr_t>(image.base), cache);
  if (took != nullptr) *took = Now() - start;
  return image.base[rva + 6];
}

void Cache(const std::string& library, std::uintptr_t rva, const std::string& dir) {
  std::string id;
  {
    Mapped file;
    if (Map(library, PROT_READ, &file)) id = SectionBuildId(file.base, file.size);
  }
  Expect(id.size() == 40, "build id " + id + " from the section headers");
  const std::string folder = dir + "/input";
  const std::string cache = folder + "/start-input";
  const std::string good = "1 " + id + " " + Hex(rva) + "\n";
  std::error_code ignored;
  std::filesystem::remove_all(folder, ignored);
  double scan = 0;
  double cached = 0;
  Expect(Start(library, cache, rva, &scan) == 4 && ReadText(cache) == good,
         "first start scans, writes the cache and starts games on mouse");
  Expect(FileByte(library, rva + 6) == 7, "the library file keeps its touch start");

  WriteText(cache, "1 " + id + " " + Hex(rva) + " kept\n");
  Expect(Start(library, cache, rva, &cached) == 4 && ReadText(cache) == "1 " + id + " " + Hex(rva) + " kept\n",
         "cached start does not scan");
  std::printf("      scan %.1f ms, cached start %.2f ms\n", scan, cached);

  WriteText(cache, "1 " + id + " none kept\n");
  Expect(Start(library, cache, rva, nullptr) == 7 && ReadText(cache) == "1 " + id + " none kept\n",
         "a build without a touch start is not scanned again");

  const std::string stale[] = {
      "1 " + id + " 1000 kept\n", "1 other-build " + Hex(rva) + " kept\n",
      "2 " + id + " " + Hex(rva) + " kept\n", "1 " + id + " zz kept\n",
      "1 " + id + " " + Hex(rva) + "z kept\n", "garbage\n", "",
  };
  for (const std::string& text : stale) {
    WriteText(cache, text);
    std::string name = text;
    if (!name.empty() && name.back() == '\n') name.pop_back();
    Expect(Start(library, cache, rva, nullptr) == 4 && ReadText(cache) == good,
           "stale cache \"" + name + "\" is scanned again");
  }
  std::filesystem::remove_all(folder, ignored);
}

}  // namespace

int main(int argc, char** argv) {
  RealBytes();
  Shapes();
  std::string first;
  std::uintptr_t first_rva = 0;
  std::string cache_dir;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--cache=", 0) == 0) {
      cache_dir = arg.substr(8);
      continue;
    }
    const std::uintptr_t rva = RealFile(arg);
    if (first.empty() && rva != 0) {
      first = arg.substr(0, arg.rfind('='));
      first_rva = rva;
    }
  }
  if (!cache_dir.empty()) {
    Images(cache_dir);
    if (!first.empty()) Cache(first, first_rva, cache_dir);
  }
  std::printf("%s\n", g_failures == 0 ? "all passed" : "FAILED");
  return g_failures == 0 ? 0 : 1;
}
