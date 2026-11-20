#pragma once
#include "Types.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <numeric>
namespace UltraArch {
// Experimental candidate generation only. Sketch equality does not establish
// content identity; exact restoration is still checked by the archive verifier.
class ContentSimilarity {
  public:
    using Groups = std::map<std::string, std::vector<FileTypeInfo>>;
    static void addSample(std::array<uint64_t, 4> &sketch, const uint8_t *data, size_t size) {
        if (!data || !size) return;
        constexpr std::array<uint64_t, 4> seeds{
            0x9e3779b97f4a7c15ULL, 0xd1b54a32d192ed03ULL,
            0x94d049bb133111ebULL, 0x8538ecfc2dff8acbULL};
        for (size_t offset = 0; offset < size; offset += 512) {
            const auto length = std::min<size_t>(512, size - offset);
            for (size_t seed = 0; seed < sketch.size(); ++seed) {
                uint64_t hash = 14695981039346656037ULL ^ seeds[seed];
                for (size_t n = 0; n < length; ++n) {
                    hash ^= data[offset + n]; hash *= 1099511628211ULL;
                }
                hash ^= length; hash *= 1099511628211ULL;
                // Avalanche before taking the minimum; zero marks missing data.
                hash ^= hash >> 30; hash *= 0xbf58476d1ce4e5b9ULL;
                hash ^= hash >> 27; hash *= 0x94d049bb133111ebULL;
                hash ^= hash >> 31;
                if (!hash) hash = 1;
                if (!sketch[seed]) sketch[seed] = std::numeric_limits<uint64_t>::max();
                sketch[seed] = std::min(sketch[seed], hash);
            }
        }
    }
    static Groups partition(const Groups &original) {
        Groups result;
        // Input extraction/session paths are physical locations, not membership
        // identity. Derive one component-wise root across every original group,
        // including categories which this policy does not partition.
        std::filesystem::path sourceRoot;
        bool rootKnown = false;
        for (const auto &[originalKey, members] : original)
            for (const auto &file : members) {
                const auto parent = file.path.lexically_normal().parent_path();
                if (!rootKnown) {
                    sourceRoot = parent; rootKnown = true;
                    continue;
                }
                std::filesystem::path common;
                auto existing = sourceRoot.begin(), next = parent.begin();
                while (existing != sourceRoot.end() && next != parent.end() && *existing == *next) {
                    common /= *existing; ++existing; ++next;
                }
                sourceRoot = std::move(common);
            }
        auto membershipPath = [&](const std::filesystem::path &physical) {
            const auto normalized = physical.lexically_normal();
            const auto relative = normalized.lexically_relative(sourceRoot);
            // Mixed roots/drive letters have no common relative representation.
            return pathUtf8(relative.empty() ? normalized : relative);
        };
        constexpr uint64_t minimumBytes = 2ULL * 1048576;
        for (const auto &[key, supplied] : original) {
            bool supported = supplied.size() >= 4 && supplied.size() <= 200000;
            uint64_t total = 0;
            for (const auto &file : supplied) {
                supported &= file.category == FileCategory::Generic_Binary ||
                             file.category == FileCategory::Text_Database;
                total = total > UINT64_MAX - file.sizeBytes ? UINT64_MAX : total + file.sizeBytes;
            }
            if (!supported || total < minimumBytes * 2) {
                result.emplace(key, supplied); continue;
            }
            auto files = supplied;
            std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
                return pathUtf8(a.path) < pathUtf8(b.path);
            });
            std::vector<size_t> parent(files.size());
            std::iota(parent.begin(), parent.end(), 0);
            auto root = [&](size_t node) {
                while (parent[node] != node) {
                    parent[node] = parent[parent[node]]; node = parent[node];
                }
                return node;
            };
            auto join = [&](size_t a, size_t b) {
                a = root(a); b = root(b);
                if (a != b) parent[std::max(a, b)] = std::min(a, b);
            };
            // Six two-component bands connect files with >=2/4 matching minima.
            // One representative per band avoids an all-pairs comparison.
            std::map<std::array<uint64_t, 5>, size_t> bands;
            std::map<std::pair<FileCategory, std::string>, size_t> duplicates;
            for (size_t i = 0; i < files.size(); ++i) {
                const auto &file = files[i];
                if (!file.contentChecksum.empty()) {
                    auto [entry, inserted] = duplicates.emplace(
                        std::make_pair(file.category, file.contentChecksum), i);
                    if (!inserted) join(i, entry->second);
                }
                for (size_t a = 0; a < 4; ++a)
                    for (size_t b = a + 1; b < 4; ++b) {
                        if (!file.similaritySketch[a] || !file.similaritySketch[b]) continue;
                        const std::array<uint64_t, 5> band{static_cast<uint64_t>(file.category),
                            a * 4 + b, file.similaritySketch[a], file.similaritySketch[b], 0};
                        auto [entry, inserted] = bands.emplace(band, i);
                        if (!inserted) join(i, entry->second);
                    }
            }
            struct Cluster { std::vector<FileTypeInfo> files; uint64_t bytes = 0; };
            std::map<size_t, Cluster> clusters;
            for (size_t i = 0; i < files.size(); ++i) {
                auto &cluster = clusters[root(i)];
                cluster.files.push_back(files[i]);
                cluster.bytes = cluster.bytes > UINT64_MAX - files[i].sizeBytes
                    ? UINT64_MAX : cluster.bytes + files[i].sizeBytes;
            }
            std::vector<Cluster *> significant;
            for (auto &[id, cluster] : clusters)
                if (cluster.files.size() >= 2 && cluster.bytes >= minimumBytes)
                    significant.push_back(&cluster);
            if (significant.size() < 2) { result.emplace(key, supplied); continue; }
            std::sort(significant.begin(), significant.end(), [](const auto *a, const auto *b) {
                return a->bytes != b->bytes ? a->bytes > b->bytes
                    : pathUtf8(a->files.front().path) < pathUtf8(b->files.front().path);
            });
            // At most seven significant clusters plus one unchanged leftovers block.
            if (significant.size() > 7) significant.resize(7);
            std::vector<FileTypeInfo> leftovers;
            auto publish = [&](std::vector<FileTypeInfo> members) {
                std::sort(members.begin(), members.end(), [](const auto &a, const auto &b) {
                    return pathUtf8(a.path) < pathUtf8(b.path);
                });
                Hasher digest;
                for (const auto &file : members) {
                    const auto name = membershipPath(file.path);
                    const auto item = std::to_string(name.size()) + ":" + name + ":" +
                                      std::to_string(file.sizeBytes) + ":" + file.contentChecksum + "\n";
                    digest.update(item.data(), item.size());
                }
                result.emplace(key + "|similarity-v1:" + digest.finish(), std::move(members));
            };
            for (auto &[id, cluster] : clusters)
                if (std::find(significant.begin(), significant.end(), &cluster) != significant.end())
                    publish(cluster.files);
                else leftovers.insert(leftovers.end(), cluster.files.begin(), cluster.files.end());
            if (!leftovers.empty()) publish(std::move(leftovers));
        }
        return result;
    }
};
} // namespace UltraArch
