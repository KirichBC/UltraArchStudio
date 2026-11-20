#pragma once
#include "ResourceGuard.hpp"
#include "Types.hpp"
#include <atomic>
#include <functional>
namespace UltraArch {
struct ProcessResult;
enum class CapabilityStatus { Supported, Unsupported, Inconclusive };
class ToolCapabilities {
  public:
    static CapabilityStatus classifyProbe(const ProcessResult &process);
    static std::vector<TaskConfig> check(const std::vector<TaskConfig> &tasks,
                                         const std::filesystem::path &tool,
                                         const std::filesystem::path &scratch, ResourceGuard *guard,
                                         const std::atomic<bool> *cancel,
                                         std::function<void(const std::string &)> log);
};
} // namespace UltraArch
