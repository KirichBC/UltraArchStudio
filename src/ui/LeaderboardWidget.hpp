#pragma once

#include "../core/Types.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace UltraArch {

class LeaderboardWidget {
  public:
    LeaderboardWidget() = default;
    std::function<bool(const std::string &)> selectCandidate, retestCandidate;
    bool commandsEnabled = false, retestEnabled = false;

    // Renders the leaderboard summary cards and interactive candidates table
    void render(const std::vector<CandidateResult> &results, const CandidateResult &bestLeader,
                uint64_t originalBytes);

  private:
    char m_filterBuffer[128] = {0};
    int m_sortColumn = 3; // default sort by size
    bool m_sortAscending = true;
    char m_exportPath[512] = {0};

    void exportToCsv(const std::vector<CandidateResult> &results,
                     const std::filesystem::path &path);
    void exportToJson(const std::vector<CandidateResult> &results,
                      const std::filesystem::path &path);
};

} // namespace UltraArch
