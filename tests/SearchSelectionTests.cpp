#include "core/SearchPlanner.hpp"
#include "core/ProbePlanner.hpp"
#include <iostream>
#include <stdexcept>
using namespace UltraArch;
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
namespace {
TaskConfig make(Codec filter, Codec backend, uint64_t dictionary, uint32_t lc = 3,
                bool sorted = false) {
    TaskConfig task;
    if (filter != backend) {
        CodecSpec front; front.codec = filter;
        task.spec.methods.push_back(front);
    }
    CodecSpec method; method.codec = backend;
    method.dictionary = MiB{dictionary}.bytes(); method.lc = lc;
    task.spec.methods.push_back(method);
    if (filter == Codec::BCJ2) {
        CodecSpec branch; branch.codec = Codec::LZMA1;
        task.spec.methods.push_back(branch); task.spec.methods.push_back(branch);
        task.spec.bindings = {"0:1", "0s1:2", "0s2:3"};
    }
    task.spec.sortByType = sorted;
    task.configId = task.spec.id();
    return task;
}
bool has(const std::vector<TaskConfig> &tasks, const TaskConfig &wanted) {
    return std::any_of(tasks.begin(), tasks.end(), [&](const auto &task) {
        return task.configId == wanted.configId;
    });
}
std::set<std::string> ids(const std::vector<TaskConfig> &tasks) {
    std::set<std::string> result;
    for (const auto &task : tasks) result.insert(task.configId);
    return result;
}
void peCoverageAndFallback() {
    std::vector<TaskConfig> matrix;
    std::vector<uint64_t> probes;
    for (auto backend : {Codec::LZMA1, Codec::LZMA2})
        for (bool sorted : {false, true})
            for (uint32_t lc : {0u, 1u, 2u, 3u, 4u})
                for (auto dictionary : {1ULL, 16ULL}) {
                    matrix.push_back(make(Codec::BCJ, backend, dictionary, lc, sorted));
                    // lc0 wins the representative sample; lc3 largest must still
                    // survive because full solid data can contain distant repeats.
                    probes.push_back(100 + lc * 100);
                }
    REQUIRE(SearchPlanner::finalists(matrix, probes, SearchMode::Balanced, 1.0).size() == matrix.size());
    auto reduced = SearchPlanner::finalists(matrix, probes, SearchMode::Fast, 1.0, true);
    REQUIRE(reduced.size() < matrix.size());
    for (auto backend : {Codec::LZMA1, Codec::LZMA2})
        for (bool sorted : {false, true}) {
            REQUIRE(has(reduced, make(Codec::BCJ, backend, 16, 0, sorted)));
            REQUIRE(has(reduced, make(Codec::BCJ, backend, 16, 3, sorted)));
        }
    probes[6] = 0;
    REQUIRE(has(SearchPlanner::finalists(matrix, probes, SearchMode::Fast, 1.0, true), matrix[6]));
    auto missing = SearchPlanner::finalists(matrix, {}, SearchMode::Balanced, 1.0, true);
    REQUIRE(missing.size() < matrix.size());
    for (auto backend : {Codec::LZMA1, Codec::LZMA2})
        for (bool sorted : {false, true})
            REQUIRE(has(missing, make(Codec::BCJ, backend, 16, 3, sorted)));
    REQUIRE(SearchPlanner::finalists(matrix, probes, SearchMode::Exhaustive, 1.0, true).size() == matrix.size());
}
void bcj2BranchBaseline() {
    std::vector<TaskConfig> matrix;
    for (uint32_t lc : {0u, 1u, 3u, 4u})
        for (uint64_t dictionary : {1ULL, 64ULL}) {
            auto task = make(Codec::BCJ2, Codec::LZMA2, dictionary, lc);
            CodecSpec branch; branch.codec = Codec::LZMA1; branch.lc = 0; branch.lp = 2;
            task.spec.methods[2] = branch; task.spec.methods[3] = branch;
            task.spec.bindings = {"0:1", "0s1:2", "0s2:3"};
            task.configId = task.spec.id(); matrix.push_back(task);
        }
    std::vector<uint64_t> probes(matrix.size(), 500);
    probes[1] = 50;
    auto selected = SearchPlanner::finalists(matrix, probes, SearchMode::Fast, 1.0, true);
    REQUIRE(has(selected, matrix[1]));
    REQUIRE(has(selected, matrix[5])); // largest default main, nondefault side streams
    REQUIRE(selected.size() < matrix.size());
}
void binaryLiteralBaseline() {
    std::vector<TaskConfig> matrix;
    for (uint64_t dictionary : {1ULL, 16ULL}) {
        auto binary = make(Codec::LZMA1, Codec::LZMA1, dictionary, 0);
        binary.spec.methods.front().pb = 0;
        binary.configId = binary.spec.id();
        binary.category = FileCategory::Generic_Binary;
        matrix.push_back(binary);
    }
    for (uint32_t lc : {1u, 2u, 3u}) matrix.push_back(make(Codec::LZMA1, Codec::LZMA1, 16, lc));
    const std::vector<uint64_t> losingProbe{1000, 1000, 10, 20, 30};
    const auto generic = SearchPlanner::finalists(matrix, losingProbe, SearchMode::Fast, 1.05);
    REQUIRE(has(generic, matrix[1]) && !has(generic, matrix[0]));
    REQUIRE(has(generic, matrix[4])); // default largest baseline remains protected
    auto text = matrix;
    for (auto &task : text) task.category = FileCategory::Text_Database;
    const auto textChosen = SearchPlanner::finalists(text, losingProbe, SearchMode::Fast, 1.05);
    REQUIRE(!has(textChosen, text[0]) && !has(textChosen, text[1]));
    REQUIRE(has(textChosen, text[4]));
}
void ppmdJointRefinement() {
    std::vector<TaskConfig> matrix;
    TaskConfig leader;
    for (uint64_t memory : {64ULL, 128ULL, 256ULL, 512ULL})
        for (uint32_t order : {8u, 10u, 12u, 16u, 20u, 24u, 32u}) {
            // Simulate an unavailable/RAM-filtered configuration: never synthesize it.
            if (memory == 128 && order == 32) continue;
            auto task = make(Codec::PPMd, Codec::PPMd, memory);
            task.spec.methods.front().order = order;
            task.configId = task.spec.id();
            if (memory == 256 && order == 12) leader = task;
            matrix.push_back(task);
        }
    auto neighbours = SearchPlanner::refinement(matrix, {leader}, leader.configId);
    auto diagonal = make(Codec::PPMd, Codec::PPMd, 128);
    diagonal.spec.methods.front().order = 24; diagonal.configId = diagonal.spec.id();
    REQUIRE(has(neighbours, diagonal));
    REQUIRE(neighbours.size() <= 8);
    for (const auto &task : neighbours) REQUIRE(has(matrix, task));
    auto absent = diagonal; absent.spec.methods.front().order = 32; absent.configId = absent.spec.id();
    REQUIRE(!has(neighbours, absent));
    auto reverse = matrix; std::reverse(reverse.begin(), reverse.end());
    REQUIRE(ids(neighbours) == ids(SearchPlanner::refinement(reverse, {leader}, leader.configId)));
    REQUIRE(!has(SearchPlanner::refinement(matrix, {leader, diagonal}, leader.configId), diagonal));
    auto orderLower = leader; orderLower.spec.methods.front().order = 10;
    orderLower.configId = orderLower.spec.id();
    auto orderUpper = leader; orderUpper.spec.methods.front().order = 16;
    orderUpper.configId = orderUpper.spec.id();
    REQUIRE(has(neighbours, orderLower) && has(neighbours, orderUpper));
    // LZMA continues to use exactly its single-coordinate immediate neighbours.
    std::vector<TaskConfig> lzma;
    for (uint32_t lc : {0u, 1u, 2u, 3u}) lzma.push_back(make(Codec::LZMA1, Codec::LZMA1, 4, lc));
    auto unchanged = SearchPlanner::refinement(lzma, {lzma[1]}, lzma[1].configId);
    REQUIRE(unchanged.size() == 2 && has(unchanged, lzma[0]) && has(unchanged, lzma[2]));
}
void tinyTournamentAndSpecialTasks() {
    std::vector<TaskConfig> matrix;
    for (auto backend : {Codec::LZMA1, Codec::LZMA2})
        for (uint32_t lc : {0u, 1u, 2u, 3u, 4u})
            for (uint64_t dictionary : {1ULL, 2ULL, 4ULL})
                {
                    auto task = make(backend, backend, dictionary, lc);
                    if (backend == Codec::LZMA2 && lc == 3) task.name = "Baseline LZMA2";
                    matrix.push_back(task);
                }
    for (uint32_t lc : {0u, 3u})
        for (uint32_t stride : {1u, 2u, 4u, 8u, 16u, 32u}) {
            auto task = make(Codec::Delta, Codec::LZMA1, 2, lc);
            task.spec.methods.front().delta = stride;
            task.configId = task.spec.id(); matrix.push_back(task);
        }
    for (uint32_t order : {10u, 12u, 16u, 20u, 24u, 32u}) {
        auto task = make(Codec::PPMd, Codec::PPMd, 16);
        task.spec.methods.front().order = order;
        task.configId = task.spec.id(); matrix.push_back(task);
    }
    matrix.push_back(make(Codec::Store, Codec::Store, 1));
    TaskConfig reused; reused.reuseInputArchive = true; reused.configId = "original";
    TaskConfig grouped; grouped.groupedSearch = true; grouped.configId = "groups";
    matrix.push_back(reused); matrix.push_back(grouped);
    auto chosen = SearchPlanner::smallInputFinalists(matrix, SearchMode::Balanced);
    REQUIRE(chosen.size() < matrix.size() / 2);
    REQUIRE(has(chosen, reused) && has(chosen, grouped) && has(chosen, matrix[matrix.size()-3]));
    REQUIRE(has(chosen, make(Codec::LZMA1, Codec::LZMA1, 4)));
    REQUIRE(has(chosen, make(Codec::LZMA2, Codec::LZMA2, 4)));
    for (uint32_t lc : {0u, 3u}) {
        auto delta = make(Codec::Delta, Codec::LZMA1, 2, lc);
        REQUIRE(has(chosen, delta));
    }
    auto reversed = matrix; std::reverse(reversed.begin(), reversed.end());
    REQUIRE(ids(chosen) == ids(SearchPlanner::smallInputFinalists(reversed, SearchMode::Balanced)));
    REQUIRE(SearchPlanner::smallInputFinalists(matrix, SearchMode::Fast).size() <= chosen.size());
    REQUIRE(ids(SearchPlanner::smallInputFinalists(matrix, SearchMode::Exhaustive)) == ids(matrix));
    // Missing or failed probes must never remove an archive/group sentinel with
    // no compressor graph; selection must not inspect methods.front().
    auto sampled = SearchPlanner::finalists({reused, grouped}, {}, SearchMode::Balanced, 1.05, true);
    REQUIRE(sampled.size() == 2);
}
void structuredMissingAndAliases() {
    std::vector<TaskConfig> matrix;
    std::vector<ProbeResult> probes;
    for (uint32_t lc : {0u, 1u, 2u, 3u, 4u})
        for (uint64_t dictionary : {1ULL, 16ULL, 64ULL, 256ULL}) {
            matrix.push_back(make(Codec::LZMA1, Codec::LZMA1, dictionary, lc));
            ProbeResult probe;
            probe.state = lc == 0 ? ProbeState::Timeout : lc == 1 ? ProbeState::Error :
                          lc == 2 ? ProbeState::ResourceRejected : ProbeState::NotStarted;
            probe.bytes = 1; // A partial result must not beat a successful sample.
            probe.sampleId = "sample-25MiB";
            probe.aliasId = "sample-capped-context-" + std::to_string(lc);
            probes.push_back(probe);
        }
    auto chosen = SearchPlanner::finalists(matrix, probes, SearchMode::Balanced, 1.05, true);
    REQUIRE(chosen.size() <= 3); // Two unknown shapes plus protected default.
    REQUIRE(has(chosen, make(Codec::LZMA1, Codec::LZMA1, 256)));
    auto reversedMatrix = matrix;
    auto reversedProbes = probes;
    std::reverse(reversedMatrix.begin(), reversedMatrix.end());
    std::reverse(reversedProbes.begin(), reversedProbes.end());
    REQUIRE(ids(chosen) == ids(SearchPlanner::finalists(reversedMatrix, reversedProbes,
                                                       SearchMode::Balanced, 1.05, true)));
    REQUIRE(ids(SearchPlanner::finalists(matrix, probes, SearchMode::Exhaustive, 1.05)) == ids(matrix));
    REQUIRE(SearchPlanner::finalists(matrix, std::vector<ProbeResult>{}, SearchMode::Fast, 1.05).size() <= 2);
    // An explicit alias shared by several contexts also gets only one fallback.
    for (auto &probe : probes) probe.aliasId = "same-encoder";
    REQUIRE(SearchPlanner::finalists(matrix, probes, SearchMode::Balanced, 1.05, true).size() <= 2);
}
void challengerCompetitionAndTies() {
    std::vector<TaskConfig> matrix{make(Codec::LZMA1, Codec::LZMA1, 16, 1)};
    std::vector<ProbeResult> probes(1);
    probes[0].state = ProbeState::Success; probes[0].bytes = 100;
    for (auto codec : {Codec::BZip2, Codec::Deflate, Codec::PPMd}) {
        auto task = make(codec, codec, 16);
        matrix.push_back(task);
        ProbeResult result; result.state = ProbeState::Success; result.bytes = 200;
        probes.push_back(result);
    }
    auto rejected = SearchPlanner::finalists(matrix, probes, SearchMode::Balanced, 1.05);
    REQUIRE(rejected.size() == 1 && has(rejected, matrix[0]));
    probes[1].bytes = 101; probes[2].bytes = 102; probes[3].bytes = 103;
    REQUIRE(SearchPlanner::finalists(matrix, probes, SearchMode::Balanced, 1.05).size() == 4);
    // Equal sizes and dictionaries must use identity rather than input order.
    matrix = {make(Codec::LZMA1, Codec::LZMA1, 16, 0),
              make(Codec::LZMA1, Codec::LZMA1, 16, 1),
              make(Codec::LZMA1, Codec::LZMA1, 16, 2)};
    probes.resize(matrix.size());
    for (auto &probe : probes) { probe.state = ProbeState::Success; probe.bytes = 100; }
    auto tie = SearchPlanner::finalists(matrix, probes, SearchMode::Fast, 1.0);
    REQUIRE(tie.size() == 1);
    const auto smallestId = std::min({matrix[0].configId, matrix[1].configId, matrix[2].configId});
    REQUIRE(tie.front().configId == smallestId);
    std::reverse(matrix.begin(), matrix.end());
    REQUIRE(ids(tie) == ids(SearchPlanner::finalists(matrix, probes, SearchMode::Fast, 1.0)));
    // Protected named baselines, Store and graph sentinels survive failed probes.
    auto low = make(Codec::LZMA2, Codec::LZMA2, 1, 4); low.name = "Baseline small";
    auto high = make(Codec::LZMA2, Codec::LZMA2, 64, 4); high.name = "Baseline large";
    auto store = make(Codec::Store, Codec::Store, 1);
    TaskConfig original; original.reuseInputArchive = true; original.configId = "original";
    matrix = {low, high, store, original};
    REQUIRE(SearchPlanner::finalists(matrix, std::vector<ProbeResult>{}, SearchMode::Fast, 1.0).size() == 4);
}
void alternateProbeWindows() {
    auto file = [](const std::string &name, uint64_t size, FileCategory category) {
        FileTypeInfo result;
        result.path = name; result.sizeBytes = size; result.category = category;
        return result;
    };
    auto signature = [](const std::vector<FileTypeInfo> &files,
                        const std::vector<ProbeFragment> &fragments, uint64_t limit) {
        std::set<std::string> result;
        uint64_t total = 0;
        for (const auto &fragment : fragments) {
            REQUIRE(fragment.fileIndex < files.size());
            const auto &source = files[fragment.fileIndex];
            REQUIRE(fragment.offset <= source.sizeBytes);
            REQUIRE(fragment.bytes <= source.sizeBytes - fragment.offset);
            REQUIRE(fragment.bytes <= limit - total);
            total += fragment.bytes;
            result.insert(pathUtf8(source.path) + ":" + std::to_string(fragment.offset) +
                          ":" + std::to_string(fragment.bytes));
        }
        return result;
    };
    std::vector<FileTypeInfo> files{file("large.dat", 10000, FileCategory::Generic_Binary)};
    const auto first = ProbePlanner::plan(files, 900);
    const auto second = ProbePlanner::plan(files, 900, 64, 1);
    REQUIRE(first.size() == 3 && second.size() == 1);
    REQUIRE(signature(files, first, 900) != signature(files, second, 900));
    REQUIRE(second[0].offset > 0 && second[0].offset + second[0].bytes < files[0].sizeBytes);
    for (uint64_t limit : {1ULL, 2ULL, 3ULL, 900ULL, 10000ULL, UINT64_MAX})
        for (uint32_t ordinal : {0u, 1u, 19u})
            signature(files, ProbePlanner::plan(files, limit, 64, ordinal), limit);
    REQUIRE(ProbePlanner::plan(files, 0, 64, 1).empty());
    REQUIRE(ProbePlanner::plan(files, 900, 0, 1).empty());
    // Alternate representatives are stable under permutations of the input.
    files.clear();
    for (uint32_t i = 0; i < 12; ++i)
        files.push_back(file(std::to_string(i) + ".dat", 1000 + i * 10,
                             FileCategory::Generic_Binary));
    const auto baseline = signature(files, ProbePlanner::plan(files, 600, 3), 600);
    const auto alternate = signature(files, ProbePlanner::plan(files, 600, 3, 1), 600);
    REQUIRE(baseline != alternate);
    std::reverse(files.begin(), files.end());
    REQUIRE(alternate == signature(files, ProbePlanner::plan(files, 600, 3, 1), 600));
    // Whole PE samples keep both end size buckets while changing representatives.
    files.clear();
    for (uint32_t i = 0; i < 12; ++i)
        files.push_back(file(std::to_string(i) + ".exe", i < 6 ? 100 + i : 1000 + i,
                             FileCategory::Executable_PE));
    const auto peFirst = ProbePlanner::plan(files, 10000, 4);
    const auto peSecond = ProbePlanner::plan(files, 10000, 4, 1);
    REQUIRE(ProbePlanner::wholePeCovered(files, peFirst));
    REQUIRE(ProbePlanner::wholePeCovered(files, peSecond));
    REQUIRE(signature(files, peFirst, 10000) != signature(files, peSecond, 10000));
    const auto peSignature = signature(files, peSecond, 10000);
    std::reverse(files.begin(), files.end());
    REQUIRE(peSignature == signature(files, ProbePlanner::plan(files, 10000, 4, 1), 10000));
    for (uint64_t limit : {1ULL, 101ULL, 1100ULL}) {
        const auto fragments = ProbePlanner::plan(files, limit, 4, 1);
        signature(files, fragments, limit);
        for (const auto &fragment : fragments)
            REQUIRE(fragment.offset == 0 && fragment.bytes == files[fragment.fileIndex].sizeBytes);
    }
    files = {file("tiny.txt", 2, FileCategory::Generic_Binary)};
    REQUIRE(signature(files, ProbePlanner::plan(files, 2), 2) ==
            signature(files, ProbePlanner::plan(files, 2, 64, 1), 2));
}
}
int main() {
    try {
        peCoverageAndFallback(); bcj2BranchBaseline(); binaryLiteralBaseline(); ppmdJointRefinement(); tinyTournamentAndSpecialTasks();
        structuredMissingAndAliases(); challengerCompetitionAndTies();
        alternateProbeWindows();
        std::cout << "[PASS] search selection contracts\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return 1;
    }
}
