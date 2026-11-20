#pragma once
#include "ArchiveAssembler.hpp"
namespace UltraArch {
class StreamingVerifier {
  public:
    enum class Result { Verified, Failed, Unavailable };
    static Result verify(const std::filesystem::path &archive, const AssemblyPlan &plan,
                         const AnalysisSummary &summary, const OptimizerSettings &settings,
                         ResourceGuard *guard, const std::atomic<bool> *cancel, std::string &error,
                         const ProcessControl *processControl = nullptr);
};
} // namespace UltraArch
