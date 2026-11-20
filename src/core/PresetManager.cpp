#include "PresetManager.hpp"
#include "SettingsJson.hpp"
#include <algorithm>
#include <fstream>
namespace UltraArch {
namespace fs = std::filesystem;
static bool safeName(const std::string &name) {
    if (name.empty() || name.size() > 120 || name == "." || name == ".." || name.back() == '.' ||
        name.back() == ' ')
        return false;
    for (unsigned char c : name)
        if (c < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos)
            return false;
    std::string stem = name.substr(0, name.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
        return false;
    if (stem.size() == 4 && (stem.substr(0, 3) == "COM" || stem.substr(0, 3) == "LPT") &&
        stem[3] >= '1' && stem[3] <= '9')
        return false;
    return true;
}
void PresetManager::initialize(const fs::path &dir, bool portableMode) {
    (void)portableMode;
    m_presetsDir = fs::absolute(dir);
    fs::create_directories(m_presetsDir);
}
std::vector<PresetInfo> PresetManager::getBuiltinPresets() {
    std::vector<PresetInfo> presets;

    PresetInfo balanced;
    balanced.name = "Balanced";
    balanced.description = "Extreme grouped search, exact content, sample selection, automatic CPU allocation. No preprocessing.";
    balanced.isBuiltin = true;
    balanced.settings.profile = OptimizationProfile::Extreme_RePack;
    balanced.settings.searchMode = SearchMode::Balanced;
    balanced.settings.enableAdaptivePruning = false;
    balanced.settings.enablePreprocessors = false;
    balanced.settings.contentPolicy = ContentPolicy::ExactBytes;
    balanced.settings.autoCpu = true;
    presets.push_back(balanced);

    PresetInfo maximum = balanced;
    maximum.name = "Maximum search (long)";
    maximum.description = "Exhaustive exact-content search without sample pruning or time budgets. Can take a long time; no universal minimum-size guarantee.";
    maximum.settings.searchMode = SearchMode::Exhaustive;
    maximum.settings.timeBudgetSeconds = 0;
    maximum.settings.processTimeoutSeconds = 0;
    presets.push_back(maximum);

    PresetInfo background = balanced;
    background.name = "Background";
    background.description = "Balanced exact-content search, automatic CPU allocation, background priority and a 25% Windows Job CPU cap.";
    background.settings.backgroundPriority = true;
    background.settings.cpuLimitPercent = 25;
    presets.push_back(background);

    // 1. Game RePack
    PresetInfo repack;
    repack.name = "Game RePack";
    repack.description =
        "Extreme Balanced search, resource preprocessing (changes stored bytes), 16 GiB RAM, 4 manual workers.";
    repack.isBuiltin = true;
    repack.settings.profile = OptimizationProfile::Extreme_RePack;
    repack.settings.enablePreprocessors = true;
    repack.settings.contentPolicy = ContentPolicy::OptimizeResources;
    repack.settings.ramLimitMB = 16384;
    repack.settings.workerThreads = 4;
    repack.settings.searchMode = SearchMode::Balanced;
    repack.settings.enableAdaptivePruning = false;
    presets.push_back(repack);

    // 2. ISO Archive
    PresetInfo iso;
    iso.name = "ISO Archive";
    iso.description = "DeepSearch, 8GB RAM, 2 workers, solidBlockSize=\"on\"";
    iso.isBuiltin = true;
    iso.settings.profile = OptimizationProfile::DeepSearch;
    iso.settings.ramLimitMB = 8192;
    iso.settings.workerThreads = 2;
    iso.settings.solidBlockSize = "on";
    presets.push_back(iso);

    // 3. Source Code
    PresetInfo src;
    src.name = "Source Code";
    src.description = "DeepSearch, 4GB RAM, 2 workers, solidBlockSize=\"e\" (group by extension)";
    src.isBuiltin = true;
    src.settings.profile = OptimizationProfile::DeepSearch;
    src.settings.ramLimitMB = 4096;
    src.settings.workerThreads = 2;
    src.settings.solidBlockSize = "e";
    presets.push_back(src);

    // 4. Photo Archive
    PresetInfo photo;
    photo.name = "Photo Archive";
    photo.description = "WinRAR_Safe, 4GB RAM, 1 worker (photos are mostly precompressed)";
    photo.isBuiltin = true;
    photo.settings.profile = OptimizationProfile::WinRAR_Safe;
    photo.settings.ramLimitMB = 4096;
    photo.settings.workerThreads = 1;
    presets.push_back(photo);

    return presets;
}

std::vector<PresetInfo> PresetManager::getUserPresets() const {
    std::vector<PresetInfo> result;
    std::error_code ec;
    if (!fs::exists(m_presetsDir, ec))
        return result;
    for (const auto &e : fs::directory_iterator(m_presetsDir)) {
        if (!e.is_regular_file() || e.path().extension() != ".json")
            continue;
        PresetInfo p;
        if (loadPreset(pathUtf8(e.path().stem()), p)) {
            p.isBuiltin = false;
            result.push_back(p);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const auto &a, const auto &b) { return a.name < b.name; });
    return result;
}
std::vector<PresetInfo> PresetManager::getAllPresets() const {
    auto p = getBuiltinPresets();
    auto u = getUserPresets();
    p.insert(p.end(), u.begin(), u.end());
    return p;
}
bool PresetManager::savePreset(const PresetInfo &p) {
    if (!safeName(p.name) || p.isBuiltin || m_presetsDir.empty())
        return false;
    try {
        validateSettings(p.settings, false);
        fs::create_directories(m_presetsDir);
        return writeAtomically(m_presetsDir / utf8Path(p.name + ".json"),
                               Json{{"schema", 2},
                                    {"name", p.name},
                                    {"description", p.description},
                                    {"settings", settingsJson(p.settings)}}
                                   .dump(2));
    } catch (...) {
        return false;
    }
}
bool PresetManager::deletePreset(const std::string &name) {
    if (!safeName(name))
        return false;
    std::error_code ec;
    return fs::remove(m_presetsDir / utf8Path(name + ".json"), ec) && !ec;
}
bool PresetManager::loadPreset(const std::string &name, PresetInfo &out) const {
    if (!safeName(name))
        return false;
    for (auto &p : getBuiltinPresets())
        if (p.name == name) {
            out = p;
            return true;
        }
    try {
        auto file = m_presetsDir / utf8Path(name + ".json");
        if (fs::file_size(file) > 1024 * 1024)
            return false;
        std::ifstream in(file);
        Json j;
        in >> j;
        PresetInfo p;
        p.name = j.at("name").get<std::string>();
        if (p.name != name)
            return false;
        p.description = j.value("description", std::string{});
        if (j.at("settings").value("enableAdaptivePruning", false))
            p.migrationWarning = j.at("settings").contains("searchMode")
                ? "Legacy adaptive toggle ignored: the preset's explicit search mode is authoritative."
                : "Legacy adaptive preset migrated to Balanced search. Review search mode before starting.";
        p.settings = settingsFromJson(j.at("settings"));
        out = p;
        return true;
    } catch (...) {
        return false;
    }
}
bool PresetManager::exportPreset(const std::string &name, const fs::path &file) const {
    PresetInfo p;
    if (!loadPreset(name, p))
        return false;
    return writeAtomically(file, Json{{"schema", 2},
                                      {"name", p.name},
                                      {"description", p.description},
                                      {"settings", settingsJson(p.settings)}}
                                     .dump(2));
}
bool PresetManager::importPreset(const fs::path &file) {
    try {
        if (fs::file_size(file) > 1024 * 1024)
            return false;
        std::ifstream in(file);
        Json j;
        in >> j;
        PresetInfo p;
        p.name = j.at("name").get<std::string>();
        p.description = j.value("description", std::string{});
        p.settings = settingsFromJson(j.at("settings"));
        return savePreset(p);
    } catch (...) {
        return false;
    }
}
} // namespace UltraArch
