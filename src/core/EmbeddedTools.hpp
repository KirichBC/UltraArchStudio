#pragma once

#include <filesystem>
#include <mutex>
#include <string>

namespace UltraArch {

class EmbeddedTools {
  public:
    // Ensures that required 7-Zip, Leanify, ECT, and SFX tools are ready for use.
    // Tools are shipped in the portable bin directory; no executable payload is extracted.
    static std::filesystem::path ensureToolsReady();

    // Gets the directory containing the verified binaries
    static std::filesystem::path getToolsDirectory();
    static std::filesystem::path getDataDirectory();
    static void verifyToolManifest(const std::filesystem::path &directory);

  private:
    static std::filesystem::path s_cachedToolsDir;
    static std::once_flag s_initFlag;
    static void initOnce();
};

} // namespace UltraArch
