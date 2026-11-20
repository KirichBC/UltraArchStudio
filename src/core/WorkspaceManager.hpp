#pragma once
#include "FileUtils.hpp"
#include <set>
namespace UltraArch {
struct ExtractionQuota {
    uint64_t totalBytes = 65536ULL * 1048576, fileBytes = 16384ULL * 1048576;
    uint32_t files = 1000000;
};
class WorkspaceManager {
  public:
    static void validateArchivePath(const std::string &path);
    static uint64_t check(const std::filesystem::path &root, const ExtractionQuota &quota);
    static void mark(const std::filesystem::path &workspace, const std::string &id);
    static bool owned(const std::filesystem::path &workspace, const std::filesystem::path &base);
};
} // namespace UltraArch
