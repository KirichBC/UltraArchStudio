#pragma once

#include "Types.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace UltraArch {

class PresetManager {
  public:
    PresetManager() = default;

    void initialize(const std::filesystem::path &presetsDir, bool portableMode = false);

    // Built-in presets
    static std::vector<PresetInfo> getBuiltinPresets();

    // User presets
    std::vector<PresetInfo> getUserPresets() const;
    std::vector<PresetInfo> getAllPresets() const; // builtin + user

    bool savePreset(const PresetInfo &preset);
    bool deletePreset(const std::string &name);
    bool loadPreset(const std::string &name, PresetInfo &outPreset) const;

    // Export/import
    bool exportPreset(const std::string &name, const std::filesystem::path &filePath) const;
    bool importPreset(const std::filesystem::path &filePath);

  private:
    std::filesystem::path m_presetsDir;
};

} // namespace UltraArch
