#include "SessionManager.hpp"
#include "SettingsJson.hpp"
#include <fstream>
namespace UltraArch {
namespace fs = std::filesystem;
static bool safeId(const std::string &id) {
    return !id.empty() && id.size() <= 128 &&
           id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
static SessionInfo parseSession(const Json &j) {
    if (j.at("schema").get<int>() != 3)
        throw std::invalid_argument("Unsupported session version");
    SessionInfo s;
    s.sessionId = j.at("sessionId").get<std::string>();
    if (!safeId(s.sessionId))
        throw std::invalid_argument("Invalid session id");
    s.inputPath = utf8Path(j.at("inputPath").get<std::string>());
    s.outputPath = utf8Path(j.at("outputPath").get<std::string>());
    s.workspace = utf8Path(j.at("workspace").get<std::string>());
    s.fingerprint = j.at("fingerprint").get<std::string>();
    s.settings = settingsFromJson(j.at("settings"));
    s.leaderTaskId = j.at("leaderTaskId").get<int>();
    s.completedTaskIds = j.at("completedTaskIds").get<std::vector<int>>();
    for (const auto &r : j.at("results"))
        s.results.push_back(resultFromJson(r));
    return s;
}
void SessionManager::setSessionDir(const fs::path &dir) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sessionDir = fs::absolute(dir);
    fs::create_directories(m_sessionDir);
}
fs::path SessionManager::getSessionFilePath(const std::string &id) const {
    return m_sessionDir / ("session_" + id + ".json");
}
bool SessionManager::saveCheckpoint(const SessionInfo &s) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sessionDir.empty() || !safeId(s.sessionId))
        return false;
    try {
        Json results = Json::array();
        for (const auto &r : s.results)
            results.push_back(resultJson(r));
        Json j = {{"schema", 3},
                  {"sessionId", s.sessionId},
                  {"inputPath", pathUtf8(s.inputPath)},
                  {"outputPath", pathUtf8(s.outputPath)},
                  {"workspace", pathUtf8(s.workspace)},
                  {"fingerprint", s.fingerprint},
                  {"settings", settingsJson(s.settings)},
                  {"leaderTaskId", s.leaderTaskId},
                  {"completedTaskIds", s.completedTaskIds},
                  {"results", results}};
        return writeAtomically(getSessionFilePath(s.sessionId), j.dump(2));
    } catch (...) {
        return false;
    }
}
bool SessionManager::tryResumeSession(const fs::path &input, const OptimizerSettings &settings,
                                      SessionInfo &out, const std::string &fingerprint,
                                      const std::string &sessionId) {
    for (const auto &s : getSessionHistory()) {
        if ((sessionId.empty() || s.sessionId == sessionId) && s.inputPath == input &&
            settingsJson(s.settings) == settingsJson(settings) && s.fingerprint == fingerprint) {
            out = s;
            return true;
        }
    }
    return false;
}
void SessionManager::clearSession(const std::string &id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!safeId(id) || m_sessionDir.empty())
        return;
    std::error_code ec;
    fs::remove(getSessionFilePath(id), ec);
}
std::vector<SessionInfo> SessionManager::getSessionHistory() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SessionInfo> result;
    if (m_sessionDir.empty() || !fs::exists(m_sessionDir))
        return result;
    for (const auto &e : fs::directory_iterator(m_sessionDir)) {
        try {
            if (!e.is_regular_file() || e.path().extension() != ".json" ||
                e.file_size() > 16 * 1024 * 1024)
                continue;
            std::ifstream in(e.path());
            Json j;
            in >> j;
            result.push_back(parseSession(j));
        } catch (...) { /* A damaged/old checkpoint must not prevent a new run. */
        }
    }
    return result;
}
std::string SessionManager::generateSessionId() {
    return uniqueId();
}
} // namespace UltraArch
