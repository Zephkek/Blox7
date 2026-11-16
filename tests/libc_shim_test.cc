#include "libc_shim/libc_shim.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace libc_shim {
namespace {

std::atomic<int> g_guest_allocations{0};

void* CountingGuestAllocator(size_t size) {
  g_guest_allocations.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(size);
}

class LibcShimTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* ca_bundle = std::getenv("BLOX7_CA_BUNDLE");
    had_ca_bundle_override_ = ca_bundle != nullptr;
    original_ca_bundle_override_ = ca_bundle == nullptr ? "" : ca_bundle;
    const char* asset_path = std::getenv("BLOX7_ASSET_PATH");
    had_asset_path_override_ = asset_path != nullptr;
    original_asset_path_override_ = asset_path == nullptr ? "" : asset_path;
    ASSERT_EQ(unsetenv("BLOX7_CA_BUNDLE"), 0);
    ASSERT_EQ(unsetenv("BLOX7_ASSET_PATH"), 0);
    ClearPathMappings();
    ConfigureGuestAllocator(nullptr);
  }
  void TearDown() override {
    ConfigureGuestAllocator(nullptr);
    ClearPathMappings();
    if (had_ca_bundle_override_) {
      EXPECT_EQ(setenv("BLOX7_CA_BUNDLE",
                       original_ca_bundle_override_.c_str(), 1),
                0);
    } else {
      EXPECT_EQ(unsetenv("BLOX7_CA_BUNDLE"), 0);
    }
    if (had_asset_path_override_) {
      EXPECT_EQ(setenv("BLOX7_ASSET_PATH",
                       original_asset_path_override_.c_str(), 1),
                0);
    } else {
      EXPECT_EQ(unsetenv("BLOX7_ASSET_PATH"), 0);
    }
  }

  bool had_ca_bundle_override_ = false;
  std::string original_ca_bundle_override_;
  bool had_asset_path_override_ = false;
  std::string original_asset_path_override_;
};

TEST_F(LibcShimTest, TranslatesRegisteredPrefix) {
  RegisterPathMapping("/data/user/0/com.roblox.client",
                      "/home/user/.var/app/org.vinegarhq.Sober/data/sober");

  const std::string input = "/data/user/0/com.roblox.client/files/logs";
  const std::string expected =
      "/home/user/.var/app/org.vinegarhq.Sober/data/sober/files/logs";

  EXPECT_EQ(TranslatePath(input), expected);
}

TEST_F(LibcShimTest, UnregisteredPathIsReturnedUnchanged) {
  const std::string input = "/proc/self/maps";
  EXPECT_EQ(TranslatePath(input), input);
}

TEST_F(LibcShimTest, TranslatesRelativeRobloxAssetPrefixes) {
  setenv("BLOX7_ASSET_PATH", "/payload/assets/content/", 1);
  Install();

  EXPECT_EQ(TranslatePath("content/guac/default.json"),
            "/payload/assets/content/guac/default.json");
  EXPECT_EQ(TranslatePath("rbx_bin/assets/content/fonts/font.json"),
            "/payload/assets/content/fonts/font.json");
  EXPECT_EQ(TranslatePath("rbx_bin/assets/shaders/mobile.pack"),
            "/payload/assets/shaders/mobile.pack");
}

TEST_F(LibcShimTest, EmptyPathIsReturnedUnchanged) {
  EXPECT_EQ(TranslatePath(""), "");
}

TEST_F(LibcShimTest, ExactPrefixMatchIsTranslated) {
  RegisterPathMapping("/data/user/0/com.roblox.client",
                      "/host/sober");

  EXPECT_EQ(TranslatePath("/data/user/0/com.roblox.client"), "/host/sober");
}

TEST_F(LibcShimTest, ShortPathIsNotTranslated) {
  RegisterPathMapping("/data/user/0/com.roblox.client", "/host/sober");

  EXPECT_EQ(TranslatePath("/data/user"), "/data/user");
}

TEST_F(LibcShimTest, SimilarPrefixWithoutPathBoundaryIsNotTranslated) {
  RegisterPathMapping("/data/user/0/com.roblox.client", "/host/sober");

  EXPECT_EQ(TranslatePath("/data/user/0/com.roblox.client.beta/cache"),
            "/data/user/0/com.roblox.client.beta/cache");
}

TEST_F(LibcShimTest, MultipleRegistrationsCoexist) {
  RegisterPathMapping("/data/user/0/com.roblox.client", "/host/sober");
  RegisterPathMapping("/sdcard/Android/data/com.roblox.client",
                      "/host/sober/sdcard");

  EXPECT_EQ(TranslatePath("/data/user/0/com.roblox.client/cache"),
            "/host/sober/cache");
  EXPECT_EQ(TranslatePath("/sdcard/Android/data/com.roblox.client/obb"),
            "/host/sober/sdcard/obb");
}

TEST_F(LibcShimTest, LongestPrefixWins) {
  RegisterPathMapping("/data/user/0/com.roblox.client", "/host/sober");
  RegisterPathMapping("/data/user/0/com.roblox.client/cache",
                      "/host/sober-cache");

  EXPECT_EQ(TranslatePath("/data/user/0/com.roblox.client/cache/tombstone.dat"),
            "/host/sober-cache/tombstone.dat");
}

TEST_F(LibcShimTest, ValidCaBundleOverrideMapsAndroidAliases) {
  char temp_template[] = "/tmp/blox7_ca_bundle_XXXXXX";
  const int fd = mkstemp(temp_template);
  ASSERT_GE(fd, 0);
  constexpr char kTestCertificate[] =
      "-----BEGIN CERTIFICATE-----\nblox7-test\n"
      "-----END CERTIFICATE-----\n";
  ASSERT_EQ(write(fd, kTestCertificate, sizeof(kTestCertificate) - 1),
            static_cast<ssize_t>(sizeof(kTestCertificate) - 1));
  ASSERT_EQ(close(fd), 0);
  ASSERT_EQ(setenv("BLOX7_CA_BUNDLE", temp_template, 1), 0);
  ASSERT_EQ(setenv("BLOX7_ASSET_PATH", "/payload/assets/content/", 1), 0);

  RegisterPathMapping("/data/user/0/com.roblox.client", "/host/sober");
  const HostCaBundleResolution resolution =
      ConfigureHostCaBundlePathMappings();

  ASSERT_TRUE(resolution.ok());
  EXPECT_TRUE(resolution.from_override);
  EXPECT_EQ(resolution.host_path, temp_template);
  EXPECT_EQ(TranslatePath(
                "/data/user/0/com.roblox.client/files/exe/cacert.pem"),
            temp_template);
  EXPECT_EQ(TranslatePath(
                "/data/data/com.roblox.client/files/exe/cacert.pem"),
            temp_template);
  EXPECT_EQ(TranslatePath("/data/user/0/com.roblox.client/files/other"),
            "/host/sober/files/other");
  for (const char* alias : {
           "ssl/cacert.pem", "content/ssl/cacert.pem",
           "rbx_bin/assets/ssl/cacert.pem", "rbx_bin/assets/content/ssl/cacert.pem",
           "/payload/assets/ssl/cacert.pem", "/payload/assets/content/ssl/cacert.pem",
           "/data/user/0/com.roblox.client/files/exe/ssl/cacert.pem",
           "/data/data/com.roblox.client/files/exe/ssl/cacert.pem"}) {
    EXPECT_EQ(TranslatePath(alias), temp_template);
  }
  FILE* mapped_file = blox7_fopen(
      "/data/user/0/com.roblox.client/files/exe/cacert.pem", "r");
  ASSERT_NE(mapped_file, nullptr);
  EXPECT_EQ(fgetc(mapped_file), '-');
  EXPECT_EQ(fclose(mapped_file), 0);

  EXPECT_EQ(unlink(temp_template), 0);
}

TEST_F(LibcShimTest, ModsReplaceRobloxMediaButNotCodeOrCertificates) {
  char temp_template[] = "/tmp/blox7_mods_XXXXXX";
  ASSERT_NE(mkdtemp(temp_template), nullptr);
  const std::filesystem::path base(temp_template);
  const std::filesystem::path assets = base / "payload/assets";
  const std::filesystem::path mods = base / "mods";
  const auto touch = [](const std::filesystem::path& file) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "blox7";
  };
  for (const char* font :
       {"content/fonts/BuilderSans-Regular.otf", "content/fonts/NotoSansThaiUI-Regular.ttf",
        "content/fonts/TwemojiMozilla.ttf", "fonts/SourceSansPro-Regular.ttf"})
    touch(assets / font);
  for (const char* file :
       {"content/textures/ui/Button.png", "ExtraContent/textures/sky/white.png",
        "ExtraContent/LuaPackages/Init.lua", "content/ssl/cacert.pem"})
    touch(mods / file);
  touch(base / "arrow.png");
  touch(base / "font.ttf");
  ASSERT_EQ(setenv("BLOX7_ASSET_PATH", (assets / "content/").c_str(), 1), 0);
  ASSERT_EQ(setenv("BLOX7_MODS_DIR", mods.c_str(), 1), 0);
  ASSERT_EQ(setenv("BLOX7_FONT_FILE", (base / "font.ttf").c_str(), 1), 0);
  ASSERT_EQ(setenv("BLOX7_CURSOR_ARROW_FILE", (base / "arrow.png").c_str(), 1), 0);
  ASSERT_EQ(setenv("BLOX7_CURSOR_HAND_FILE", (base / "hand.png").c_str(), 1), 0);

  ConfigureModPathMappings();

  const std::string arrow = "content/textures/Cursors/KeyboardMouse/ArrowFarCursor.png";
  for (const std::string& alias :
       {arrow, "rbx_bin/assets/" + arrow, (assets / arrow).string()})
    EXPECT_EQ(TranslatePath(alias), (base / "arrow.png").string());
  const std::string hand = "content/textures/Cursors/KeyboardMouse/ArrowCursor.png";
  EXPECT_EQ(TranslatePath(hand), hand);
  EXPECT_EQ(TranslatePath("content/textures/ui/Button.png"),
            (mods / "content/textures/ui/Button.png").string());
  EXPECT_EQ(TranslatePath("rbx_bin/assets/ExtraContent/textures/sky/white.png"),
            (mods / "ExtraContent/textures/sky/white.png").string());
  for (const char* untouched :
       {"rbx_bin/assets/ExtraContent/LuaPackages/Init.lua", "content/ssl/cacert.pem",
        "content/fonts/NotoSansThaiUI-Regular.ttf", "content/fonts/TwemojiMozilla.ttf"})
    EXPECT_EQ(TranslatePath(untouched), untouched);
  EXPECT_EQ(TranslatePath("content/fonts/BuilderSans-Regular.otf"),
            (base / "font.ttf").string());
  EXPECT_EQ(TranslatePath("rbx_bin/assets/fonts/SourceSansPro-Regular.ttf"),
            (base / "font.ttf").string());
  char asset[512];
  ASSERT_TRUE(blox7_mod_file(arrow.c_str(), asset, sizeof asset));
  EXPECT_EQ(std::string(asset), (base / "arrow.png").string());
  ASSERT_TRUE(blox7_mod_file("content/fonts/BuilderSans-Regular.otf", asset, sizeof asset));
  EXPECT_EQ(std::string(asset), (base / "font.ttf").string());
  EXPECT_FALSE(blox7_mod_file("content/ssl/cacert.pem", asset, sizeof asset));
  EXPECT_FALSE(blox7_mod_file(hand.c_str(), asset, sizeof asset));

  for (const char* variable : {"BLOX7_MODS_DIR", "BLOX7_FONT_FILE",
                               "BLOX7_CURSOR_ARROW_FILE", "BLOX7_CURSOR_HAND_FILE"})
    EXPECT_EQ(unsetenv(variable), 0);
  std::filesystem::remove_all(base);
}

TEST_F(LibcShimTest, SystemCaBundleIsResolvedAsAbsoluteRegularFile) {
  const HostCaBundleResolution resolution = ResolveHostCaBundle();

  ASSERT_TRUE(resolution.ok());
  EXPECT_FALSE(resolution.from_override);
  ASSERT_FALSE(resolution.host_path.empty());
  EXPECT_EQ(resolution.host_path.front(), '/');
  struct stat file_status {};
  ASSERT_EQ(stat(resolution.host_path.c_str(), &file_status), 0);
  EXPECT_TRUE(S_ISREG(file_status.st_mode));
  EXPECT_GT(file_status.st_size, 0);
  EXPECT_EQ(access(resolution.host_path.c_str(), R_OK), 0);
}

TEST_F(LibcShimTest, RelativeCaBundleOverrideFailsWithoutSystemFallback) {
  ASSERT_EQ(setenv("BLOX7_CA_BUNDLE", "relative/cacert.pem", 1), 0);

  const HostCaBundleResolution resolution = ResolveHostCaBundle();

  EXPECT_EQ(resolution.status, HostCaBundleStatus::kInvalidOverride);
  EXPECT_TRUE(resolution.from_override);
  EXPECT_TRUE(resolution.host_path.empty());
}

TEST_F(LibcShimTest, MissingCaBundleOverrideFailsWithoutSystemFallback) {
  ASSERT_EQ(setenv("BLOX7_CA_BUNDLE",
                   "/tmp/blox7-ca-bundle-does-not-exist", 1),
            0);

  const HostCaBundleResolution resolution = ResolveHostCaBundle();

  EXPECT_EQ(resolution.status, HostCaBundleStatus::kInvalidOverride);
  EXPECT_TRUE(resolution.host_path.empty());
}

TEST_F(LibcShimTest, NonRegularCaBundleOverrideFailsClosed) {
  char temp_template[] = "/tmp/blox7_ca_directory_XXXXXX";
  char* directory = mkdtemp(temp_template);
  ASSERT_NE(directory, nullptr);
  ASSERT_EQ(setenv("BLOX7_CA_BUNDLE", directory, 1), 0);

  const HostCaBundleResolution resolution = ResolveHostCaBundle();

  EXPECT_EQ(resolution.status, HostCaBundleStatus::kInvalidOverride);
  EXPECT_TRUE(resolution.host_path.empty());
  EXPECT_EQ(rmdir(directory), 0);
}

TEST_F(LibcShimTest, UnreadableCaBundleOverrideFailsClosed) {
  char temp_template[] = "/tmp/blox7_ca_unreadable_XXXXXX";
  const int fd = mkstemp(temp_template);
  ASSERT_GE(fd, 0);
  constexpr char kContent[] = "not-readable";
  ASSERT_EQ(write(fd, kContent, sizeof(kContent) - 1),
            static_cast<ssize_t>(sizeof(kContent) - 1));
  ASSERT_EQ(close(fd), 0);
  ASSERT_EQ(chmod(temp_template, 0000), 0);
  ASSERT_EQ(setenv("BLOX7_CA_BUNDLE", temp_template, 1), 0);

  const HostCaBundleResolution resolution = ResolveHostCaBundle();

  EXPECT_EQ(resolution.status, HostCaBundleStatus::kInvalidOverride);
  EXPECT_TRUE(resolution.host_path.empty());
  EXPECT_EQ(unlink(temp_template), 0);
}

TEST_F(LibcShimTest, FilesystemCapacityCallsTranslateAndroidPaths) {
  char temp_template[] = "/tmp/blox7_libc_shim_test_XXXXXX";
  char* root = mkdtemp(temp_template);
  ASSERT_NE(root, nullptr);

  const std::string root_path = root;
  const std::string storage_path = root_path + "/rbx-storage";
  ASSERT_EQ(mkdir(storage_path.c_str(), 0755), 0);

  RegisterPathMapping("/data/user/0/com.roblox.client", root_path);

  struct statvfs vfs {};
  EXPECT_EQ(blox7_statvfs("/data/user/0/com.roblox.client/rbx-storage",
                             &vfs),
            0);
  EXPECT_GT(vfs.f_bsize, 0U);

  struct statfs fs {};
  EXPECT_EQ(blox7_statfs("/data/user/0/com.roblox.client/rbx-storage", &fs),
            0);
  EXPECT_GT(fs.f_bsize, 0);

  EXPECT_EQ(rmdir(storage_path.c_str()), 0);
  EXPECT_EQ(rmdir(root_path.c_str()), 0);
}

TEST_F(LibcShimTest, RealpathUsesConfiguredGuestAllocatorForOwnedResult) {
  g_guest_allocations.store(0, std::memory_order_relaxed);
  ConfigureGuestAllocator(CountingGuestAllocator);

  char* result = blox7_realpath("/tmp", nullptr);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(std::string(result), "/tmp");
  EXPECT_EQ(g_guest_allocations.load(std::memory_order_relaxed), 1);
  std::free(result);
}

TEST_F(LibcShimTest, ClearRemovesAllMappings) {
  RegisterPathMapping("/data/user/0/com.roblox.client", "/host/sober");
  ClearPathMappings();

  const std::string input = "/data/user/0/com.roblox.client/files";
  EXPECT_EQ(TranslatePath(input), input);
}

}  // namespace
}  // namespace libc_shim
