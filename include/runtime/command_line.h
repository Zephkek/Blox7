#ifndef BLOX7_RUNTIME_COMMAND_LINE_H_
#define BLOX7_RUNTIME_COMMAND_LINE_H_

#include <string>
#include <vector>

namespace blox7 {
namespace runtime {

enum class CommandMode {
  kRun,
  kHelp,
};

enum class WindowMode {
  kUnspecified,
  kHeadless,
  kWindowed,
};

struct CommandLineOptions {
  CommandMode mode = CommandMode::kRun;
  WindowMode window_mode = WindowMode::kUnspecified;
  std::string program_name = "blox7";
  std::string roblox_library_path;
  std::string graphics_backend;
  std::string raw_launch_argument;
  std::string launch_request_json;
  int launch_argument_index = -1;
  bool allow_unverified_build = false;
  bool force_run_latest = false;
};

struct CommandLineParseResult {
  CommandLineOptions options;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

CommandLineParseResult ParseCommandLine(int argc, const char* const argv[]);
bool BuildCommandLineReexecArguments(const CommandLineOptions& options,
                                     int argc, const char* const argv[],
                                     std::vector<std::string>* arguments,
                                     std::string* error);
void ScrubCommandLineLaunchArguments(CommandLineOptions* options, int argc,
                                     char* argv[]);
bool ApplyCommandLineEnvironment(const CommandLineOptions& options,
                                 std::string* error);
std::string CommandLineUsage(const std::string& program_name);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_COMMAND_LINE_H_
