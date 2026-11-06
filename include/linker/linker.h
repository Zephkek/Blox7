#ifndef BLOX7_LINKER_LINKER_H_
#define BLOX7_LINKER_LINKER_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace linker {

using LibraryHandle = void*;
using SymbolMap = std::unordered_map<std::string, void*>;
using AndroidLibraryLoadObserver = bool (*)(void* context,
                                            std::string_view logical_name,
                                            std::uintptr_t image_base);

struct ProgramHeaderValidation {
  bool valid = false;
  std::string error;

  explicit operator bool() const noexcept { return valid; }
};

LibraryHandle AddSyntheticLibrary(const std::string& soname,
                                  const SymbolMap& exports);

void RegisterSyntheticSymbol(const std::string& soname,
                             const std::string& name, void* address);

void RegisterBionicPthreadKeyRuntimeForLibc();
void RegisterBionicAtForkRuntimeForLibc();
void RegisterBionicLargeFileRuntimeForLibc();
void RegisterBionicHostLibcRuntimeForLibc();
void RegisterBionicSysconfRuntimeForLibc();
void RegisterBionicSignalRuntimeForLibc();
void RegisterBionicStdioRuntimeForLibc();

void RegisterBionicDynamicLoaderForLibdl();

SymbolMap GetSyntheticLibrarySymbols(const std::string& soname);

void UpdateAndroidLibraryPath(const std::string& search_path);

bool SetAndroidLibraryLoadObserver(AndroidLibraryLoadObserver observer,
                                   void* context);
void ClearAndroidLibraryLoadObserver(void* context);

LibraryHandle OpenAndroidLibrary(const std::string& real_path,
                                 const std::string& logical_name);

LibraryHandle LoadLibrary(const std::string& real_path,
                          const std::string& host_alias);

void* ResolveSymbol(LibraryHandle handle, const std::string& symbol_name);

bool IsBlockedRobloxCrashReportNativeSymbol(
    std::string_view symbol_name) noexcept;

void UnloadLibrary(LibraryHandle handle);

void RegisterSymbol(const std::string& name, void* addr);

const SymbolMap& GetBionicSymbols();

void* BionicDlIteratePhdrAddress();

ProgramHeaderValidation ValidateBionicProgramHeaders(LibraryHandle handle);

std::uintptr_t FindLoadedAndroidLibraryBase(const std::string& logical_name);

const std::unordered_map<std::string, LibraryHandle>& GetSymbolTable();

}  // namespace linker

#endif  // BLOX7_LINKER_LINKER_H_
