#pragma once
#include "Types.hpp"
#include <algorithm>
#include <initializer_list>
#include <map>
#include <set>
namespace UltraArch {
enum class ProbeState { Success, Timeout, Error, ResourceRejected, NotStarted };
struct ProbeResult {
    ProbeState state = ProbeState::NotStarted;
    uint64_t bytes = 0;
    double seconds = 0;
    std::string sampleId;
    std::string aliasId;
    bool coverageComplete = false;
    uint64_t coveredBytes = 0;
    uint64_t totalBytes = 0;
};
class SearchPlanner {
    static std::string shape(CompressionSpec spec) {
        for (auto &method : spec.methods)
            if (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2 ||
                method.codec == Codec::PPMd)
                method.dictionary = MiB{1}.bytes();
        return spec.id();
    }
    static uint64_t dictionarySize(const CompressionSpec &spec) {
        uint64_t result = 0;
        for (const auto &method : spec.methods)
            if (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2 ||
                method.codec == Codec::PPMd)
                result += method.dictionary.value;
        return result;
    }
    static std::string probeFamily(CompressionSpec spec) {
        if (spec.methods.empty())
            return "Empty";
        if (spec.methods.front().codec == Codec::BCJ || spec.methods.front().codec == Codec::BCJ2) {
            // Rank contexts within a complete graph/backend/solid/order variant.
            // A good two-stream result must not crowd out a four-stream graph.
            for (auto &method : spec.methods)
                if (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2) {
                    method.lc = 3; method.lp = 0; method.pb = 2;
                    method.fastBytes = 273; method.matchCycles = 0;
                    method.matchFinder.clear();
                }
            return "PE:" + shape(std::move(spec));
        }
        if (spec.methods.front().codec != Codec::Delta)
            return spec.family();
        // A short sample must not let one backend/context consume every Delta slot.
        // Strides compete within the same graph; dictionaries remain sample aliases.
        for (auto &method : spec.methods)
            if (method.codec == Codec::Delta)
                method.delta = 1;
        return "Delta:" + shape(std::move(spec));
    }
    static bool protectedTask(const TaskConfig &task) {
        return task.reuseInputArchive || task.groupedSearch || task.spec.methods.empty();
    }
    static bool defaultLzma(const CodecSpec &method) {
        return (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2) &&
               method.lc == 3 && method.lp == 0 && method.pb == 2 &&
               method.fastBytes == 273 && !method.matchCycles && method.matchFinder.empty();
    }
    static void preserveBaselines(const std::vector<TaskConfig> &tasks, std::set<size_t> &keep) {
        std::map<std::string, size_t> largest;
        size_t first = tasks.size(), last = tasks.size();
        for (size_t i = 0; i < tasks.size(); ++i) {
            const auto &task = tasks[i];
            if (protectedTask(task)) {
                keep.insert(i);
                continue;
            }
            if (task.spec.methods.front().codec == Codec::Store)
                keep.insert(i);
            if (task.name.rfind("Baseline", 0) == 0) {
                const auto bytes = dictionarySize(task.spec);
                if (first == tasks.size() || bytes < dictionarySize(tasks[first].spec) ||
                    (bytes == dictionarySize(tasks[first].spec) && task.configId < tasks[first].configId))
                    first = i;
                if (last == tasks.size() || bytes > dictionarySize(tasks[last].spec) ||
                    (bytes == dictionarySize(tasks[last].spec) && task.configId < tasks[last].configId))
                    last = i;
            }
            const bool pe = task.spec.methods.front().codec == Codec::BCJ ||
                            task.spec.methods.front().codec == Codec::BCJ2;
            if (!pe && task.spec.methods.size() != 1)
                continue;
            // For PE protect the default main stream while preserving the entire
            // branch graph; side streams legitimately use nondefault contexts.
            const size_t main = pe ? 1 : 0;
            if (main >= task.spec.methods.size())
                continue;
            const auto &mainMethod = task.spec.methods[main];
            // Binary data can prefer a context independent of literal position.
            // A small probe cannot establish that this loses on a full solid stream.
            const bool binaryLiteralBaseline = !pe && task.spec.methods.size() == 1 &&
                task.category == FileCategory::Generic_Binary && mainMethod.codec == Codec::LZMA1 &&
                mainMethod.lc == 0 && mainMethod.lp == 0 && mainMethod.pb == 0 &&
                mainMethod.fastBytes == 273 && !mainMethod.matchCycles && mainMethod.matchFinder.empty();
            if (!defaultLzma(mainMethod) && !binaryLiteralBaseline)
                continue;
            const auto key = shape(task.spec);
            const auto found = largest.find(key);
            if (found == largest.end() || dictionarySize(task.spec) > dictionarySize(tasks[found->second].spec) ||
                (dictionarySize(task.spec) == dictionarySize(tasks[found->second].spec) &&
                 task.configId < tasks[found->second].configId))
                largest[key] = i;
        }
        if (first < tasks.size()) keep.insert(first);
        if (last < tasks.size()) keep.insert(last);
        for (const auto &[key, i] : largest) keep.insert(i);
    }
    static std::vector<TaskConfig> selected(const std::vector<TaskConfig> &tasks, const std::set<size_t> &keep) {
        std::vector<TaskConfig> result;
        for (size_t i = 0; i < tasks.size(); ++i)
            if (keep.count(i)) result.push_back(tasks[i]);
        return result;
    }
  public:
    static std::vector<TaskConfig> finalists(const std::vector<TaskConfig> &tasks,
                                             const std::vector<uint64_t> &probeBytes,
                                             SearchMode mode, double threshold, bool trustedWholePe = false) {
        std::vector<ProbeResult> probes;
        probes.reserve(probeBytes.size());
        for (auto bytes : probeBytes) {
            ProbeResult result;
            result.state = bytes ? ProbeState::Success : ProbeState::NotStarted;
            result.bytes = bytes;
            probes.push_back(std::move(result));
        }
        return finalists(tasks, probes, mode, threshold, trustedWholePe);
    }
    static std::vector<TaskConfig> finalists(const std::vector<TaskConfig> &tasks,
                                             std::initializer_list<uint64_t> probeBytes,
                                             SearchMode mode, double threshold, bool trustedWholePe = false) {
        return finalists(tasks, std::vector<uint64_t>(probeBytes), mode, threshold, trustedWholePe);
    }
    static std::vector<TaskConfig> finalists(const std::vector<TaskConfig> &tasks,
                                             const std::vector<ProbeResult> &probes,
                                             SearchMode mode, double threshold, bool trustedWholePe = false) {
        if (mode == SearchMode::Exhaustive)
            return tasks;
        std::set<size_t> keep;
        std::map<std::string, std::vector<size_t>> families;
        for (size_t i = 0; i < tasks.size(); ++i) {
            if (protectedTask(tasks[i])) {
                keep.insert(i);
                continue;
            }
            families[probeFamily(tasks[i].spec)].push_back(i);
            // Filters need complete PE/media structure; do not discard based on truncated probes.
            if (!trustedWholePe && (tasks[i].spec.family() == "BCJ" || tasks[i].spec.family() == "BCJ2"))
                keep.insert(i);
        }
        // Only completed samples are comparable. A partial archive from a timeout
        // is not a successful compression result, even when it has a byte count.
        const auto bytes = [&](size_t i) {
            return i < probes.size() && probes[i].state == ProbeState::Success ? probes[i].bytes : uint64_t(0);
        };
        size_t perFamily = mode == SearchMode::Fast ? 1 : 2;
        uint64_t best = UINT64_MAX;
        for (size_t i = 0; i < tasks.size(); ++i)
            if (bytes(i)) best = std::min(best, bytes(i));
        for (auto &[family, indices] : families) {
            std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                if (bool(bytes(a)) != bool(bytes(b))) return bool(bytes(a));
                if (bytes(a) != bytes(b)) return bytes(a) < bytes(b);
                const auto da = dictionarySize(tasks[a].spec), db = dictionarySize(tasks[b].spec);
                if (da != db) return da > db;
                return tasks[a].configId < tasks[b].configId;
            });
            std::set<std::string> shapes;
            std::vector<size_t> representatives;
            for (auto i : indices)
                if (bytes(i) && shapes.insert(shape(tasks[i].spec)).second)
                    representatives.push_back(i);
            const auto firstCodec = tasks[indices.front()].spec.methods.front().codec;
            const bool challenger = firstCodec == Codec::BZip2 || firstCodec == Codec::Deflate ||
                                    firstCodec == Codec::PPMd;
            const auto competitive = [&](size_t i) {
                return best == UINT64_MAX || static_cast<long double>(bytes(i)) <=
                    static_cast<long double>(best) * std::max(1.0, threshold);
            };
            for (size_t n = 0; n < std::min(perFamily, representatives.size()); ++n)
                if (!challenger || competitive(representatives[n])) keep.insert(representatives[n]);
            // Inconclusive probes remain eligible, but do not expand every
            // sample-capped dictionary alias into a full-data encoding. Keep a
            // bounded, reproducible fallback per family; this is heuristic only.
            std::set<std::pair<std::string, std::string>> unknownAliases;
            std::set<std::string> unknownShapes;
            size_t unknownCount = 0;
            for (auto i : indices) {
                if (bytes(i) || unknownCount >= perFamily) continue;
                const auto identity = shape(tasks[i].spec);
                if (!unknownShapes.insert(identity).second) continue;
                if (i < probes.size() && !probes[i].aliasId.empty() &&
                    !unknownAliases.emplace(probes[i].sampleId, probes[i].aliasId).second) continue;
                keep.insert(i);
                ++unknownCount;
            }
            if (mode == SearchMode::Balanced) {
                // Limit close sample ties; full-data neighbours are tested in refinement.
                size_t close = 0;
                for (auto i : representatives)
                    if (bytes(i) && competitive(i) && close++ < 3)
                        keep.insert(i);
            }
        }
        preserveBaselines(tasks, keep);
        return selected(tasks, keep);
    }
    // Whole-data seed tournament for tiny inputs: skip a duplicate sampling pass,
    // retain representative contexts, then let normal full-data refinement expand
    // around its verified winner. This is heuristic; Exhaustive remains untouched.
    static std::vector<TaskConfig> smallInputFinalists(const std::vector<TaskConfig> &tasks,
                                                      SearchMode mode) {
        if (mode == SearchMode::Exhaustive)
            return tasks;
        std::set<size_t> keep;
        std::map<std::string, std::vector<size_t>> families;
        for (size_t i = 0; i < tasks.size(); ++i)
            if (protectedTask(tasks[i])) keep.insert(i);
            else families[probeFamily(tasks[i].spec)].push_back(i);
        const size_t seeds = mode == SearchMode::Fast ? 2 : 4;
        auto preference = [](const CompressionSpec &spec) {
            uint64_t distance = 0;
            for (const auto &method : spec.methods) {
                if (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2) {
                    distance += (method.lc > 3 ? method.lc - 3 : 3 - method.lc) + method.lp;
                    distance += (method.pb > 2 ? method.pb - 2 : 2 - method.pb);
                    distance += method.fastBytes == 273 ? 0 : 8;
                    distance += method.matchCycles || !method.matchFinder.empty() ? 8 : 0;
                }
                if (method.codec == Codec::Delta)
                    distance += method.delta == 1 ? 0 : method.delta == 4 ? 1 : method.delta == 2 ? 2 : 3;
                if (method.codec == Codec::PPMd)
                    distance += method.order == 12 ? 0 : method.order == 24 ? 1 : method.order == 32 ? 2 : 3;
            }
            return distance;
        };
        for (auto &[family, indices] : families) {
            std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                const auto pa = preference(tasks[a].spec), pb = preference(tasks[b].spec);
                if (pa != pb) return pa < pb;
                const auto da = dictionarySize(tasks[a].spec), db = dictionarySize(tasks[b].spec);
                if (da != db) return da > db;
                return tasks[a].configId < tasks[b].configId;
            });
            std::set<std::string> shapes;
            size_t count = 0;
            for (auto i : indices)
                if (shapes.insert(shape(tasks[i].spec)).second && count++ < seeds)
                    keep.insert(i);
        }
        preserveBaselines(tasks, keep);
        return selected(tasks, keep);
    }
    static std::vector<TaskConfig> refinement(const std::vector<TaskConfig> &matrix,
                                              const std::vector<TaskConfig> &tested,
                                              const std::string &leaderId) {
        auto leader = std::find_if(matrix.begin(), matrix.end(),
                                   [&](const auto &t) { return t.configId == leaderId; });
        if (leader == matrix.end())
            return {};
        std::vector<TaskConfig> extra;
        auto append = [&](const TaskConfig *task) {
            if (task && extra.size() < 8 &&
                std::none_of(tested.begin(), tested.end(),
                             [&](const auto &t) { return t.configId == task->configId; }) &&
                std::none_of(extra.begin(), extra.end(),
                             [&](const auto &t) { return t.configId == task->configId; }))
                extra.push_back(*task);
        };
        // PPMd order and model memory interact: coordinate-only steps can miss
        // a smaller-memory/higher-order winner within the two-round budget.
        // Probe only existing eligible diagonal pairs; the global cap remains 8.
        for (size_t methodIndex = 0; methodIndex < leader->spec.methods.size(); ++methodIndex) {
            const auto &center = leader->spec.methods[methodIndex];
            if (center.codec != Codec::PPMd)
                continue;
            auto normalized = [methodIndex](auto spec) {
                spec.methods[methodIndex].order = 8;
                spec.methods[methodIndex].dictionary = MiB{1}.bytes();
                return spec.id();
            };
            const auto identity = normalized(leader->spec);
            std::set<uint32_t> orders;
            uint64_t lowerMemory = 0;
            std::vector<const TaskConfig *> eligible;
            for (const auto &task : matrix) {
                if (protectedTask(task) || task.spec.methods.size() != leader->spec.methods.size() ||
                    normalized(task.spec) != identity)
                    continue;
                eligible.push_back(&task);
                const auto &method = task.spec.methods[methodIndex];
                orders.insert(method.order);
                if (method.dictionary.value < center.dictionary.value)
                    lowerMemory = std::max(lowerMemory, method.dictionary.value);
            }
            if (orders.empty())
                continue;
            std::vector<uint32_t> numericOrders(orders.begin(), orders.end());
            const auto quartile = numericOrders[(3 * (numericOrders.size() - 1) + 3) / 4];
            for (auto order : {quartile, numericOrders.back()}) {
                if (order <= center.order)
                    continue;
                for (auto memory : {lowerMemory, center.dictionary.value}) {
                    if (!memory)
                        continue;
                    const TaskConfig *candidate = nullptr;
                    for (const auto *task : eligible) {
                        const auto &method = task->spec.methods[methodIndex];
                        if (method.order == order && method.dictionary.value == memory &&
                            (!candidate || task->configId < candidate->configId))
                            candidate = task;
                    }
                    append(candidate);
                }
            }
        }
        // Neighbours differ in one numeric coordinate, not in matrix row position.
        // Keep the graph, solid/order policy and all other coordinates unchanged.
        auto neighbours = [&](size_t methodIndex, auto value, auto normalized) {
            const auto identity = normalized(leader->spec).id();
            const auto center = value(leader->spec.methods[methodIndex]);
            const TaskConfig *lower = nullptr, *upper = nullptr;
            for (const auto &task : matrix) {
                if (task.reuseInputArchive || task.groupedSearch ||
                    task.spec.methods.size() != leader->spec.methods.size() ||
                    normalized(task.spec).id() != identity)
                    continue;
                const auto coordinate = value(task.spec.methods[methodIndex]);
                if (coordinate < center &&
                    (!lower || coordinate > value(lower->spec.methods[methodIndex])))
                    lower = &task;
                if (coordinate > center &&
                    (!upper || coordinate < value(upper->spec.methods[methodIndex])))
                    upper = &task;
            }
            append(lower);
            append(upper);
        };
        for (size_t i = 0; i < leader->spec.methods.size(); ++i) {
            const auto codec = leader->spec.methods[i].codec;
            std::vector<std::pair<uint32_t CodecSpec::*, uint32_t>> coordinates;
            if (codec == Codec::LZMA1 || codec == Codec::LZMA2)
                coordinates = {{&CodecSpec::lc, 0}, {&CodecSpec::lp, 0},
                               {&CodecSpec::pb, 0}, {&CodecSpec::fastBytes, 5},
                               {&CodecSpec::matchCycles, 0}};
            if (codec == Codec::PPMd)
                coordinates = {{&CodecSpec::order, 8}};
            if (codec == Codec::Delta)
                coordinates = {{&CodecSpec::delta, 1}};
            for (const auto &[member, canonical] : coordinates)
                neighbours(i, [member](const auto &method) { return method.*member; },
                           [i, member, canonical](auto spec) {
                               spec.methods[i].*member = canonical;
                               return spec;
                           });
            if (codec == Codec::LZMA1 || codec == Codec::LZMA2 || codec == Codec::PPMd)
                neighbours(i, [](const auto &method) { return method.dictionary.value; },
                           [i](auto spec) {
                               spec.methods[i].dictionary = MiB{1}.bytes();
                               return spec;
                           });
        }
        return extra;
    }
};
} // namespace UltraArch
