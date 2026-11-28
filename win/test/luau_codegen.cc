#include <cstdint>
#include <cstdio>
#include <string>

#include "runtime/luau_codegen_policy.h"

namespace {

using blox7::runtime::AvxState;
using blox7::runtime::DetectAvxState;
using blox7::runtime::MergeLuauCodegenClientSettingsOverrides;

constexpr char kBase[] =
    R"({"DFIntTaskSchedulerTargetFps":"240","FFlagUseRuntimeMutexRvHttpClient":"False"})";
constexpr char kForced[] =
    R"({"DFIntTaskSchedulerTargetFps":"240","FFlagDebugDisableCodegen":"True","FFlagLuauCodegen":"False",)"
    R"("FFlagUseRuntimeMutexRvHttpClient":"False"})";
constexpr char kOdd[] = "{ \"b\" : 1 ,\n  \"a\":\"x\",\"FFlagLuauCodegen\":\"True\" }";

int g_failures = 0;

void Expect(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++g_failures;
}

AvxState State(bool cpu, bool osxsave, std::uint64_t xcr0) {
  AvxState avx;
  avx.cpu = cpu;
  avx.osxsave = osxsave;
  avx.xcr0 = xcr0;
  return avx;
}

bool Merge(const AvxState& avx, const std::string& base, std::string* out) {
  std::string error;
  out->clear();
  return MergeLuauCodegenClientSettingsOverrides(avx, base, out, &error);
}

void Forced(const AvxState& avx, const std::string& what) {
  std::string out;
  Expect(avx.luau_codegen_off(), what + ": policy on");
  Expect(Merge(avx, kBase, &out) && out == kForced, what + ": both keys added, the rest kept");
}

void Untouched(const AvxState& avx, const std::string& what) {
  std::string out;
  Expect(!avx.luau_codegen_off(), what + ": policy off");
  Expect(Merge(avx, kBase, &out) && out == kBase, what + ": settings byte-identical");
  Expect(Merge(avx, kOdd, &out) && out == kOdd, what + ": odd spacing and a user value byte-identical");
  Expect(Merge(avx, "", &out) && out.empty(), what + ": empty settings stay empty");
}

void Malformed(const AvxState& avx, const std::string& what) {
  for (const char* bad : {"{", "[]", "not json", "{\"FFlagLuauCodegen\":}", "\"True\""}) {
    std::string out = "unchanged", error;
    const bool ok = MergeLuauCodegenClientSettingsOverrides(avx, bad, &out, &error);
    Expect(!ok && !error.empty() && out == "unchanged", what + ": " + bad + " is refused");
  }
}

}  // namespace

int main() {
  Forced(State(true, true, 0x3), "cpu avx, xcr0 0x3");
  Forced(State(true, false, 0), "cpu avx, osxsave off");
  Forced(State(true, true, 0x5), "cpu avx, xcr0 without sse state");
  Untouched(State(true, true, 0x7), "cpu avx, xcr0 0x7");
  Untouched(State(true, true, 0xe7), "cpu avx-512, xcr0 0xe7");
  Untouched(State(false, false, 0), "no avx, osxsave off");
  Untouched(State(false, true, 0x3), "no avx, xcr0 0x3");

  std::string out;
  Expect(Merge(State(true, true, 0x3),
               R"({"FFlagLuauCodegen":"True","FFlagDebugDisableCodegen":"False","FIntX":"1"})", &out) &&
             out == R"({"FFlagDebugDisableCodegen":"True","FFlagLuauCodegen":"False","FIntX":"1"})",
         "user codegen values are overridden");
  Expect(Merge(State(true, true, 0x3), kOdd, &out) &&
             out == R"({"FFlagDebugDisableCodegen":"True","FFlagLuauCodegen":"False","a":"x","b":1})",
         "a user FFlagLuauCodegen True is overridden in odd spacing");
  Expect(Merge(State(true, true, 0x3), "", &out) &&
             out == R"({"FFlagDebugDisableCodegen":"True","FFlagLuauCodegen":"False"})",
         "empty settings get both keys");
  Malformed(State(true, true, 0x3), "policy on");
  Malformed(State(true, true, 0x7), "policy off");
  std::string error;
  Expect(!MergeLuauCodegenClientSettingsOverrides(State(true, true, 0x3), kBase, nullptr, &error) && !error.empty(),
         "a missing output is refused");

  const AvxState host = DetectAvxState();
  Expect(host.osxsave || host.xcr0 == 0, "this cpu: xcr0 is only read with osxsave");
  std::printf("      this cpu: avx=%d osxsave=%d xcr0=0x%llx policy=%s\n", host.cpu, host.osxsave,
              static_cast<unsigned long long>(host.xcr0), host.luau_codegen_off() ? "on" : "off");
  std::printf("%s\n", g_failures == 0 ? "all passed" : "FAILED");
  return g_failures == 0 ? 0 : 1;
}
