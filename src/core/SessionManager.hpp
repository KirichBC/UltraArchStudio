#pragma once

#include "Types.hpp"
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace UltraArch {

class SessionManager {
  public:
    SessionManager() = default;

    // Set directory for session files
    void setSessionDir(const std::filesystem::path &dir);

    // Save current state
    bool saveCheckpoint(const SessionInfo &info);

    // Try to find and load a compatible session for the given input
    bool tryResumeSession(const std::filesystem::path &inputPath, const OptimizerSettings &settings,
                          SessionInfo &outInfo, const std::string &fingerprint = "",
                          const std::string &sessionId = "");

    // Clear session data
    void clearSession(const std::string &sessionId);

    // Get history of all past sessions
    std::vector<SessionInfo> getSessionHistory() const;

    // Generate unique session ID
    static std::string generateSessionId();

  private:
    std::filesystem::path m_sessionDir;

    std::filesystem::path getSessionFilePath(const std::string &sessionId) const;

    mutable std::mutex m_mutex;
};

} // namespace UltraArch
