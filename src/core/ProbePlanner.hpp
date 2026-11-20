#pragma once
#include "Types.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace UltraArch {
struct ProbeFragment {
    size_t fileIndex;
    uint64_t offset, bytes;
    std::string suffix;
};
class ProbePlanner {
  public:
    static bool wholePeCovered(const std::vector<FileTypeInfo> &files,
                               const std::vector<ProbeFragment> &fragments) {
        if (files.empty() || fragments.empty()) return false;
        uint64_t smallest = UINT64_MAX, largest = 0;
        for (const auto &file : files) {
            if (file.category != FileCategory::Executable_PE) return false;
            if (file.sizeBytes) {
                smallest = std::min(smallest, file.sizeBytes);
                largest = std::max(largest, file.sizeBytes);
            }
        }
        auto bucket = [](uint64_t value) { uint32_t n = 0; while (value >>= 1) ++n; return n; };
        bool low = false, high = false;
        std::set<size_t> whole;
        for (const auto &fragment : fragments) {
            if (fragment.fileIndex >= files.size()) return false;
            const auto &file = files[fragment.fileIndex];
            if (fragment.offset || fragment.bytes != file.sizeBytes) return false;
            whole.insert(fragment.fileIndex);
            low |= bucket(file.sizeBytes) == bucket(smallest);
            high |= bucket(file.sizeBytes) == bucket(largest);
        }
        return largest && low && high && whole.size() >= std::min<size_t>(2, files.size());
    }
    static std::vector<ProbeFragment> plan(const std::vector<FileTypeInfo> &files,
                                           uint64_t limit, size_t maxFiles = 64,
                                           uint32_t sampleOrdinal = 0) {
        if (!limit || !maxFiles)
            return {};
        struct Stratum {
            FileCategory category;
            std::vector<size_t> indices;
            uint64_t bytes = 0;
        };
        std::map<std::pair<FileCategory, std::string>, Stratum> strata;
        for (size_t i = 0; i < files.size(); ++i) {
            if (!files[i].sizeBytes)
                continue;
            auto extension = pathUtf8(files[i].path.extension());
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            auto &s = strata[{files[i].category, extension}];
            s.category = files[i].category;
            s.indices.push_back(i);
            s.bytes += files[i].sizeBytes;
        }
        std::vector<Stratum *> ordered;
        for (auto &[key, s] : strata) {
            std::sort(s.indices.begin(), s.indices.end(), [&](size_t a, size_t b) {
                return files[a].sizeBytes == files[b].sizeBytes
                    ? pathUtf8(files[a].path) < pathUtf8(files[b].path)
                    : files[a].sizeBytes < files[b].sizeBytes;
            });
            ordered.push_back(&s);
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const auto *a, const auto *b) { return a->bytes > b->bytes; });
        std::vector<Stratum *> selected;
        std::set<FileCategory> categories;
        const auto maxStrata = std::min<size_t>(16, maxFiles);
        for (auto *s : ordered)
            if (selected.size() < maxStrata && categories.insert(s->category).second)
                selected.push_back(s);
        for (auto *s : ordered)
            if (selected.size() < maxStrata &&
                std::find(selected.begin(), selected.end(), s) == selected.end())
                selected.push_back(s);
        if (selected.empty())
            return {};
        std::vector<size_t> chosen;
        for (size_t i = 0; i < selected.size(); ++i) {
            const auto &indices = selected[i]->indices;
            auto count = std::min(indices.size(), maxFiles / selected.size() +
                                                (i < maxFiles % selected.size() ? 1 : 0));
            for (size_t n = 0; n < count; ++n) {
                auto rank = count == 1 ? indices.size() - 1
                                      : n * (indices.size() - 1) / (count - 1);
                if (sampleOrdinal && indices.size() > count) {
                    const auto original = rank;
                    const auto upper = n + 1 < count
                        ? (rank + (n + 1) * (indices.size() - 1) / (count - 1)) / 2
                        : indices.size() - 1;
                    const auto lower = n
                        ? ((n - 1) * (indices.size() - 1) / (count - 1) + rank) / 2 + 1 : 0;
                    if (rank < upper)
                        ++rank;
                    else if (rank > lower)
                        --rank;
                    // PE endpoints retain their original size bucket so coverage
                    // guards cannot be weakened by choosing alternate files.
                    if (selected[i]->category == FileCategory::Executable_PE &&
                        (n == 0 || n + 1 == count)) {
                        auto bucket = [](uint64_t value) {
                            uint32_t result = 0;
                            while (value >>= 1) ++result;
                            return result;
                        };
                        if (bucket(files[indices[rank]].sizeBytes) !=
                            bucket(files[indices[original]].sizeBytes))
                            rank = original;
                    }
                }
                chosen.push_back(indices[rank]);
            }
        }
        // Redistribute bytes unused by small files. PE samples always remain whole;
        // large PE files still retain BCJ/BCJ2 candidates for full-input testing.
        std::vector<uint64_t> allocated(chosen.size(), 0);
        uint64_t unused = limit;
        const auto perFile = limit / chosen.size();
        for (size_t i = 0; i < chosen.size(); ++i) {
            const auto &f = files[chosen[i]];
            if (f.category != FileCategory::Executable_PE || f.sizeBytes <= perFile)
                allocated[i] = std::min(f.sizeBytes, perFile);
            unused -= allocated[i];
        }
        while (unused) {
            size_t eligible = 0;
            for (size_t i = 0; i < chosen.size(); ++i) {
                const auto &f = files[chosen[i]];
                if (allocated[i] < f.sizeBytes &&
                    (f.category != FileCategory::Executable_PE || f.sizeBytes <= unused))
                    ++eligible;
            }
            if (!eligible)
                break;
            const auto share = std::max<uint64_t>(1, unused / eligible);
            bool advanced = false;
            for (size_t i = 0; i < chosen.size() && unused; ++i) {
                const auto &f = files[chosen[i]];
                auto extra = std::min({f.sizeBytes - allocated[i], share, unused});
                if (f.category == FileCategory::Executable_PE) {
                    if (allocated[i] || f.sizeBytes > unused)
                        continue;
                    extra = f.sizeBytes;
                }
                if (extra) {
                    allocated[i] += extra;
                    unused -= extra;
                    advanced = true;
                }
            }
            if (!advanced)
                break;
        }
        std::vector<ProbeFragment> result;
        for (size_t i = 0; i < chosen.size(); ++i) {
            auto index = chosen[i];
            auto bytes = allocated[i];
            if (!bytes)
                continue;
            if (bytes == files[index].sizeBytes)
                result.push_back({index, 0, bytes, ""});
            else if (sampleOrdinal) {
                // A second contiguous interior window complements the first
                // sample's dispersed start/middle/end fragments.
                const auto free = files[index].sizeBytes - bytes;
                const auto numerator = uint64_t(sampleOrdinal % 5) + 1;
                const auto offset = (free / 6) * numerator + (free % 6) * numerator / 6;
                result.push_back({index, offset, bytes, "-window-" + std::to_string(sampleOrdinal)});
            } else {
                auto part = bytes / 3;
                if (part) {
                    result.push_back({index, 0, part, "-start"});
                    result.push_back({index, (files[index].sizeBytes - part) / 2, part, "-middle"});
                }
                auto tail = bytes - 2 * part;
                result.push_back({index, files[index].sizeBytes - tail, tail, "-end"});
            }
        }
        return result;
    }
};
} // namespace UltraArch
