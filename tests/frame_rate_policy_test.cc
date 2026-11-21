#include "runtime/frame_rate_policy.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace blox7 {
namespace runtime {
namespace {

TEST(FrameRatePolicyTest, ParsesSupportedModes) {
  EXPECT_EQ(ParseFrameRatePolicy("").mode,
            FrameRateLimitMode::kUnmanaged);
  EXPECT_EQ(ParseFrameRatePolicy("-1").mode,
            FrameRateLimitMode::kUnmanaged);
  EXPECT_EQ(ParseFrameRatePolicy("display").mode, FrameRateLimitMode::kDisplay);
  EXPECT_EQ(ParseFrameRatePolicy("10").fixed_fps, 10);
  EXPECT_EQ(ParseFrameRatePolicy("70").fixed_fps, 70);
  EXPECT_EQ(ParseFrameRatePolicy("144").fixed_fps, 144);
  EXPECT_EQ(ParseFrameRatePolicy("unlimited").mode,
            FrameRateLimitMode::kUnlimited);
  EXPECT_EQ(ParseFrameRatePolicy("999").fixed_fps, 999);
  EXPECT_FALSE(ParseFrameRatePolicy("0").valid());
  EXPECT_FALSE(ParseFrameRatePolicy("-10").valid());
  EXPECT_FALSE(ParseFrameRatePolicy("10fps").valid());
  EXPECT_FALSE(ParseFrameRatePolicy("10 ").valid());
}

TEST(FrameRatePolicyTest, UnmanagedOnlyEnablesRobloxBasicSettingsCap) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("-1"),
      R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})", &merged,
      &error))
      << error;
  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.at("FFlagGameBasicSettingsFramerateCap5"), "True");
  EXPECT_FALSE(parsed.contains("DFIntTaskSchedulerTargetFps"));
  EXPECT_FALSE(parsed.contains("FFlagTaskSchedulerLimitTargetFpsTo2402"));
  EXPECT_EQ(parsed.at("FStringGraphicsVulkanShaderMTDenyPattern"), "4318:.*");
}

TEST(FrameRatePolicyTest, MergesArbitraryFixedValueIntoSchedulerFflag) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("10"), "{}", &merged, &error));
  EXPECT_NE(merged.find("\"DFIntTaskSchedulerTargetFps\":\"10\""),
            std::string::npos);
  EXPECT_NE(merged.find("\"FFlagGameBasicSettingsFramerateCap5\":\"True\""),
            std::string::npos);
}

TEST(FrameRatePolicyTest, MergesUnlimitedWithoutLosingGraphicsPolicy) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("unlimited"),
      R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})", &merged,
      &error));
  EXPECT_NE(merged.find("DFIntTaskSchedulerTargetFps\":\"240"),
            std::string::npos);
  EXPECT_EQ(merged.find("DFIntTaskSchedulerTargetFps\":\"0"),
            std::string::npos);
  EXPECT_EQ(merged.find("FFlagTaskSchedulerLimitTargetFpsTo2402"),
            std::string::npos);
  EXPECT_NE(merged.find("FStringGraphicsVulkanShaderMTDenyPattern"),
            std::string::npos);
}

TEST(FrameRatePolicyTest, LeavesSchedulerOutWhenRobloxSettingHoldsTheCap) {
  FrameRatePolicy policy = ParseFrameRatePolicy("unlimited");
  policy.held_by_roblox_setting = true;
  std::string merged;
  std::string error;
  ASSERT_TRUE(
      MergeFrameRateClientSettingsOverrides(policy, "{}", &merged, &error));
  EXPECT_EQ(merged.find("DFIntTaskSchedulerTargetFps"), std::string::npos);
  EXPECT_NE(merged.find("FFlagGameBasicSettingsFramerateCap5"),
            std::string::npos);
}

TEST(FrameRatePolicyTest, OnlyLimitsAboveTheSchedulerCeilingUseRobloxSetting) {
  EXPECT_EQ(RobloxFramerateCapFor(ParseFrameRatePolicy("unlimited")),
            kUnlimitedRobloxFramerateCap);
  EXPECT_EQ(RobloxFramerateCapFor(ParseFrameRatePolicy("300")), 300);
  EXPECT_FALSE(RobloxFramerateCapFor(ParseFrameRatePolicy("240")));
  EXPECT_FALSE(RobloxFramerateCapFor(ParseFrameRatePolicy("60")));
  EXPECT_FALSE(RobloxFramerateCapFor(ParseFrameRatePolicy("-1")));
  EXPECT_FALSE(RobloxFramerateCapFor(ParseFrameRatePolicy("display")));
}

class RobloxFramerateCapTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("blox7-framerate-" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "appData");
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  void WriteSettings(const std::string &name, const std::string &cap_line) {
    std::ofstream(root_ / "appData" / name, std::ios::binary)
        << "<roblox>\n\t<Item class=\"UserGameSettings\">\n\t\t<Properties>\n"
        << cap_line << "\t\t\t<bool name=\"Fullscreen\">false</bool>\n"
        << "\t\t</Properties>\n\t</Item>\n</roblox>\n";
  }
  std::string Settings(const std::string &name) {
    std::ifstream input(root_ / "appData" / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  bool Sync(std::optional<int> cap) {
    std::string error;
    return SyncRobloxFramerateCap(root_ / "appData", root_ / "loan", cap,
                                  &error);
  }

  std::filesystem::path root_;
};

TEST_F(RobloxFramerateCapTest, NeedsRobloxToHaveSavedItsSettings) {
  EXPECT_FALSE(Sync(9999));
  EXPECT_TRUE(Sync(std::nullopt));
}

TEST_F(RobloxFramerateCapTest, LendsAndReturnsThePlayersCap) {
  WriteSettings("GlobalBasicSettings_13.xml",
                "\t\t\t<int name=\"FramerateCap\">144</int>\n");
  ASSERT_TRUE(Sync(9999));
  EXPECT_NE(Settings("GlobalBasicSettings_13.xml")
                .find("<int name=\"FramerateCap\">9999</int>"),
            std::string::npos);
  ASSERT_TRUE(Sync(300));
  ASSERT_TRUE(Sync(std::nullopt));
  EXPECT_NE(Settings("GlobalBasicSettings_13.xml")
                .find("<int name=\"FramerateCap\">144</int>"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(root_ / "loan"));
}

TEST_F(RobloxFramerateCapTest, KeepsACapThePlayerPickedInGame) {
  WriteSettings("GlobalBasicSettings_13.xml",
                "\t\t\t<int name=\"FramerateCap\">-1</int>\n");
  ASSERT_TRUE(Sync(9999));
  WriteSettings("GlobalBasicSettings_13.xml",
                "\t\t\t<int name=\"FramerateCap\">60</int>\n");
  ASSERT_TRUE(Sync(std::nullopt));
  EXPECT_NE(Settings("GlobalBasicSettings_13.xml")
                .find("<int name=\"FramerateCap\">60</int>"),
            std::string::npos);
}

TEST_F(RobloxFramerateCapTest, AddsTheCapToTheNewestSettingsFile) {
  WriteSettings("GlobalBasicSettings_9.xml", "");
  WriteSettings("GlobalBasicSettings_13.xml", "");
  ASSERT_TRUE(Sync(9999));
  EXPECT_NE(Settings("GlobalBasicSettings_13.xml")
                .find("\t\t<Properties>\n\t\t\t<int "
                      "name=\"FramerateCap\">9999</int>\n"),
            std::string::npos);
  EXPECT_EQ(Settings("GlobalBasicSettings_9.xml").find("FramerateCap"),
            std::string::npos);
  ASSERT_TRUE(Sync(std::nullopt));
  EXPECT_NE(Settings("GlobalBasicSettings_13.xml")
                .find("<int name=\"FramerateCap\">-1</int>"),
            std::string::npos);
}

TEST_F(RobloxFramerateCapTest, LeavesAnUnreadableCapAlone) {
  WriteSettings("GlobalBasicSettings_13.xml",
                "\t\t\t<int name=\"FramerateCap\">abc</int>\n");
  EXPECT_FALSE(Sync(9999));
  EXPECT_NE(Settings("GlobalBasicSettings_13.xml")
                .find("<int name=\"FramerateCap\">abc</int>"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(root_ / "loan"));
}

TEST(FrameRatePolicyTest, RejectsMalformedAndConflictingOverrides) {
  std::string merged;
  std::string error;
  EXPECT_FALSE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("120"), "not-json", &merged, &error));
  EXPECT_FALSE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("120"), R"({"DFIntTaskSchedulerTargetFps":"60"})",
      &merged, &error));
}

} // namespace
} // namespace runtime
} // namespace blox7
