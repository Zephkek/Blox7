#include "runtime/roblox_start_input.h"

#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>

namespace blox7 {
namespace runtime {
namespace {

constexpr std::size_t kReach = 0x100;
constexpr std::size_t kStoreSize = 10;
constexpr std::uint8_t kRexB = 0x41;
constexpr std::uint32_t kMouseMovement = 4;
constexpr std::uint32_t kTouch = 7;
constexpr std::uint32_t kGamepad1 = 12;
constexpr char kCacheVersion[] = "1";

struct Store {
  std::size_t value_at = 0;
  std::uint8_t rex = 0;
  std::uint8_t modrm = 0;
  std::uint32_t disp = 0;
  std::uint32_t value = 0;

  bool SameField(const Store& other) const {
    return rex == other.rex && modrm == other.modrm && disp == other.disp;
  }
};

std::optional<Store> StoreAt(const std::uint8_t* code, std::size_t size,
                             std::size_t at) {
  Store store;
  std::size_t opcode = at;
  if (opcode < size && code[opcode] == kRexB) {
    store.rex = kRexB;
    ++opcode;
  }
  if (opcode >= size || size - opcode < kStoreSize || code[opcode] != 0xC7 ||
      (code[opcode + 1] & 0xF8) != 0x80 || (code[opcode + 1] & 7) == 4) {
    return std::nullopt;
  }
  store.modrm = code[opcode + 1];
  std::memcpy(&store.disp, code + opcode + 2, sizeof store.disp);
  std::memcpy(&store.value, code + opcode + 6, sizeof store.value);
  store.value_at = opcode + 6;
  return store;
}

struct Branch {
  std::size_t next = 0;
  std::size_t target = 0;
};

std::optional<Branch> BranchAt(const std::uint8_t* code, std::size_t size,
                               std::size_t at) {
  if (at >= size || size - at < 2) {
    return std::nullopt;
  }
  Branch branch;
  std::int64_t offset = 0;
  if (code[at] == 0x74 || code[at] == 0x75) {
    branch.next = at + 2;
    offset = static_cast<std::int8_t>(code[at + 1]);
  } else if (size - at >= 6 && code[at] == 0x0F &&
             (code[at + 1] == 0x84 || code[at + 1] == 0x85)) {
    branch.next = at + 6;
    std::int32_t rel32 = 0;
    std::memcpy(&rel32, code + at + 2, sizeof rel32);
    offset = rel32;
  } else {
    return std::nullopt;
  }
  const std::int64_t target = static_cast<std::int64_t>(branch.next) + offset;
  if (target < 0 || static_cast<std::uint64_t>(target) >= size) {
    return std::nullopt;
  }
  branch.target = static_cast<std::size_t>(target);
  return branch;
}

bool IsX86Image(const Elf64_Ehdr* header) {
  return std::memcmp(header->e_ident, ELFMAG, SELFMAG) == 0 &&
         header->e_ident[EI_CLASS] == ELFCLASS64 &&
         header->e_machine == EM_X86_64 &&
         header->e_phentsize == sizeof(Elf64_Phdr);
}

bool Loaded(const Elf64_Phdr* segments, std::size_t count,
            const Elf64_Phdr& part) {
  for (std::size_t i = 0; i < count; ++i) {
    const Elf64_Phdr& load = segments[i];
    if (load.p_type == PT_LOAD && part.p_vaddr >= load.p_vaddr &&
        part.p_vaddr - load.p_vaddr <= load.p_filesz &&
        part.p_filesz <= load.p_filesz - (part.p_vaddr - load.p_vaddr)) {
      return true;
    }
  }
  return false;
}

std::string BuildId(std::uintptr_t base, const Elf64_Phdr* segments,
                    std::size_t count) {
  static const char kHex[] = "0123456789abcdef";
  for (std::size_t i = 0; i < count; ++i) {
    if (segments[i].p_type != PT_NOTE ||
        !Loaded(segments, count, segments[i])) {
      continue;
    }
    const auto* at =
        reinterpret_cast<const std::uint8_t*>(base + segments[i].p_vaddr);
    const std::uint8_t* const end = at + segments[i].p_filesz;
    while (static_cast<std::size_t>(end - at) >= sizeof(Elf64_Nhdr)) {
      Elf64_Nhdr note;
      std::memcpy(&note, at, sizeof note);
      const std::size_t name_size =
          (std::size_t{note.n_namesz} + 3) & ~std::size_t{3};
      const std::size_t desc_size =
          (std::size_t{note.n_descsz} + 3) & ~std::size_t{3};
      if (name_size + desc_size >
          static_cast<std::size_t>(end - at) - sizeof note) {
        break;
      }
      const std::uint8_t* name = at + sizeof note;
      const std::uint8_t* desc = name + name_size;
      at = desc + desc_size;
      if (note.n_type != NT_GNU_BUILD_ID || note.n_namesz != 4 ||
          std::memcmp(name, "GNU", 4) != 0) {
        continue;
      }
      std::string id;
      for (std::size_t k = 0; k < note.n_descsz; ++k) {
        id += kHex[desc[k] >> 4];
        id += kHex[desc[k] & 15];
      }
      return id;
    }
  }
  return {};
}

struct CachedSite {
  bool known = false;
  std::optional<std::uintptr_t> rva;
};

CachedSite ReadCache(const std::filesystem::path& file,
                     const std::string& build_id) {
  CachedSite cached;
  std::ifstream input(file);
  std::string version;
  std::string id;
  std::string site;
  if (!(input >> version >> id >> site) || version != kCacheVersion ||
      id != build_id) {
    return cached;
  }
  if (site == "none") {
    cached.known = true;
    return cached;
  }
  char* end = nullptr;
  const unsigned long long rva = std::strtoull(site.c_str(), &end, 16);
  if (end != site.c_str() && *end == '\0') {
    cached.known = true;
    cached.rva = static_cast<std::uintptr_t>(rva);
  }
  return cached;
}

void WriteCache(const std::filesystem::path& file, const std::string& build_id,
                std::optional<std::uintptr_t> rva) {
  std::error_code error;
  std::filesystem::create_directories(file.parent_path(), error);
  std::filesystem::path temporary = file;
  temporary += ".tmp." + std::to_string(getpid());
  bool written = false;
  {
    std::ofstream output(temporary, std::ios::trunc);
    output << kCacheVersion << ' ' << build_id << ' ';
    if (rva) {
      output << std::hex << *rva;
    } else {
      output << "none";
    }
    output << '\n';
    written = static_cast<bool>(output.flush());
  }
  if (written) {
    std::filesystem::rename(temporary, file, error);
  }
  if (!written || error) {
    std::filesystem::remove(temporary, error);
  }
}

bool WriteMouseMovement(std::uint8_t* value) {
  const auto page_size = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
  void* page = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(value) &
                                       ~(page_size - 1));
  if (mprotect(page, page_size, PROT_READ | PROT_WRITE) != 0) {
    return false;
  }
  *value = kMouseMovement;
  __builtin___clear_cache(reinterpret_cast<char*>(value),
                          reinterpret_cast<char*>(value + 1));
  return mprotect(page, page_size, PROT_READ | PROT_EXEC) == 0;
}

}  // namespace

bool IsTouchStart(const std::uint8_t* code, std::size_t size, std::size_t at) {
  const std::optional<Store> touch = StoreAt(code, size, at);
  if (!touch || touch->value != kTouch) {
    return false;
  }
  const std::size_t from = at > kReach ? at - kReach : 0;
  const std::size_t to = size - at > kReach ? at + kReach : size;
  for (std::size_t jump = from; jump < to; ++jump) {
    const std::optional<Branch> branch = BranchAt(code, size, jump);
    if (!branch || (branch->target != at && branch->next != at)) {
      continue;
    }
    const std::optional<Store> gamepad = StoreAt(
        code, size, branch->target == at ? branch->next : branch->target);
    if (gamepad && gamepad->value == kGamepad1 && gamepad->SameField(*touch)) {
      return true;
    }
  }
  return false;
}

std::optional<std::size_t> FindTouchStart(const std::uint8_t* code,
                                          std::size_t size) {
  std::optional<std::size_t> found;
  bool twice = false;
  const auto consider = [&](std::size_t at) {
    if (IsTouchStart(code, size, at)) {
      twice = twice || found.has_value();
      found = at;
    }
  };
  for (std::size_t opcode = 0; opcode < size; ++opcode) {
    const void* hit = std::memchr(code + opcode, 0xC7, size - opcode);
    if (hit == nullptr) {
      break;
    }
    opcode =
        static_cast<std::size_t>(static_cast<const std::uint8_t*>(hit) - code);
    consider(opcode);
    if (opcode > 0 && code[opcode - 1] == kRexB) {
      consider(opcode - 1);
    }
  }
  return twice ? std::nullopt : found;
}

void StartGamesOnMouse(std::uintptr_t base,
                       const std::filesystem::path& cache_file) {
  const auto* header = reinterpret_cast<const Elf64_Ehdr*>(base);
  if (!IsX86Image(header)) {
    return;
  }
  const auto* segments =
      reinterpret_cast<const Elf64_Phdr*>(base + header->e_phoff);
  const Elf64_Phdr* text = nullptr;
  for (std::size_t i = 0; i < header->e_phnum && text == nullptr; ++i) {
    if (segments[i].p_type == PT_LOAD && (segments[i].p_flags & PF_X) != 0) {
      text = &segments[i];
    }
  }
  if (text == nullptr) {
    return;
  }
  auto* code = reinterpret_cast<std::uint8_t*>(base + text->p_vaddr);
  const std::size_t size = text->p_filesz;
  const std::string build_id = BuildId(base, segments, header->e_phnum);
  const CachedSite cached =
      build_id.empty() ? CachedSite{} : ReadCache(cache_file, build_id);
  std::optional<std::size_t> at;
  if (cached.rva && *cached.rva >= text->p_vaddr &&
      IsTouchStart(code, size, *cached.rva - text->p_vaddr)) {
    at = *cached.rva - text->p_vaddr;
  } else if (!cached.known || cached.rva) {
    at = FindTouchStart(code, size);
    if (!build_id.empty()) {
      WriteCache(cache_file, build_id,
                 at ? std::optional<std::uintptr_t>(text->p_vaddr + *at)
                    : std::nullopt);
    }
  }
  if (!at) {
    std::fprintf(stderr,
                 "  [input] games start with touch input, no touch start "
                 "found in this Roblox build\n");
    return;
  }
  const auto rva = static_cast<std::size_t>(text->p_vaddr + *at);
  const std::optional<Store> touch = StoreAt(code, size, *at);
  if (!touch || !WriteMouseMovement(code + touch->value_at)) {
    std::fprintf(stderr,
                 "  [input] games start with touch input, +0x%zx cannot be "
                 "written\n",
                 rva);
    return;
  }
  std::fprintf(stderr, "  [input] games start with mouse input (+0x%zx)\n",
               rva);
}

}  // namespace runtime
}  // namespace blox7
