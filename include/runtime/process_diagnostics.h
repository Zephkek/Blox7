#ifndef BLOX7_RUNTIME_PROCESS_DIAGNOSTICS_H_
#define BLOX7_RUNTIME_PROCESS_DIAGNOSTICS_H_

namespace blox7::runtime {

enum class ProcessDiagnosticStage { kStartup, kNativeRuntime, kShutdown };

void InstallCpuLimitDiagnostics();
void LogProcessDiagnostics(ProcessDiagnosticStage stage);

}  // namespace blox7::runtime

#endif  // BLOX7_RUNTIME_PROCESS_DIAGNOSTICS_H_
