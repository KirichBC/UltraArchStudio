#pragma once
#include "Types.hpp"
#include "ContentSimilarity.hpp"
#include <algorithm>
#include <map>
namespace UltraArch {
class GroupPlanner {
  public:
    static std::map<std::string, std::vector<FileTypeInfo>> plan(const AnalysisSummary &summary,
                                                               bool similarity = false) {
        std::map<std::string, std::vector<FileTypeInfo>> groups;
        for (const auto &file : summary.files) {
            auto extension = pathUtf8(file.path.extension());
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            // Share long-range matches across executable/container extensions. Media and
            // text keep their extension-specific contexts and Delta stride candidates.
            auto key = std::to_string(static_cast<int>(file.category)) + ":";
            if (file.category == FileCategory::Executable_PE)
                key += "executables";
            else if (file.category == FileCategory::Precompressed_HighEntropy)
                key += "containers";
            else
                key += extension;
            groups[key].push_back(file);
        }
        // Small binary extension groups cannot use a large dictionary. Merge them to
        // recover cross-file matches and avoid an independent header for each suffix.
        std::vector<FileTypeInfo> small;
        for (auto it = groups.begin(); it != groups.end();) {
            uint64_t bytes = 0;
            for (const auto &file : it->second)
                bytes += file.sizeBytes;
            if (it->first.rfind("4:", 0) == 0 && bytes <= 1048576) {
                small.insert(small.end(), it->second.begin(), it->second.end());
                it = groups.erase(it);
            } else
                ++it;
        }
        if (!small.empty())
            groups["4:small-binary"] = std::move(small);
        return similarity ? ContentSimilarity::partition(groups) : groups;
    }
};
} // namespace UltraArch
