#include "core/CostModel.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace UltraArch;
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("ultraarch-cost-model-" + uniqueId());
    std::filesystem::create_directory(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{root};
    try {
        const auto file = root / "cost.json";
        AnalysisSummary input; input.totalBytes = 40ULL * 1048576;
        input.dominantCategory = FileCategory::Generic_Binary;
        TaskConfig task; task.spec.methods = {CodecSpec{Codec::LZMA2}};
        task.configId = task.spec.id();
        CostModel cold(file, "backend-with-dll-A");
        const auto fallback = cold.estimateSeconds(input, task);
        REQUIRE(std::isfinite(fallback) && fallback > 0);
        CandidateResult result; result.configId = task.configId; result.originalBytes = input.totalBytes;
        result.durationSeconds = 100; result.status = CandidateStatus::Verified;
        cold.record(input, {result});
        REQUIRE(cold.estimateSeconds(input, task) == 100);
        REQUIRE(cold.save());
        CostModel warm(file, "backend-with-dll-A");
        REQUIRE(warm.estimateSeconds(input, task) == 100);
        auto larger = input; larger.totalBytes *= 2;
        REQUIRE(warm.estimateSeconds(larger, task) == 200);
        CostModel otherBackend(file, "backend-with-dll-B");
        REQUIRE(otherBackend.estimateSeconds(input, task) == fallback);
        auto otherContext = task; otherContext.spec.methods.front().lc = 0;
        otherContext.configId = otherContext.spec.id();
        REQUIRE(warm.estimateSeconds(input, otherContext) == cold.estimateSeconds(input, otherContext));
        REQUIRE(warm.estimateSeconds(input, otherContext) != 100);
        auto otherCategory = input; otherCategory.dominantCategory = FileCategory::Text_Database;
        REQUIRE(warm.estimateSeconds(otherCategory, task) == fallback);
        auto hugeInput = input; hugeInput.totalBytes *= 32;
        REQUIRE(warm.estimateSeconds(hugeInput, task) != 3200); // no distant extrapolation
        for (auto invalid : {-1.0, 0.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN(), 1e12}) {
            result.durationSeconds = invalid; warm.record(input, {result});
        }
        result.durationSeconds = 500; result.status = CandidateStatus::OOM_Terminated;
        warm.record(input, {result});
        REQUIRE(warm.estimateSeconds(input, task) == 100);
        Json malformed = {{"schema", 1}, {"records", Json::object()}};
        const auto key = "backend-with-dll-A:" + std::to_string(static_cast<int>(input.dominantCategory)) +
                         ":" + task.configId + ":26";
        malformed["records"][key] = {{"bytes", input.totalBytes}, {"seconds", -100}};
        { std::ofstream out(file); out << malformed.dump(); }
        CostModel rejected(file, "backend-with-dll-A");
        REQUIRE(rejected.estimateSeconds(input, task) == fallback);
        { std::ofstream out(file); out << "{ broken"; }
        CostModel corrupt(file, "backend-with-dll-A");
        REQUIRE(corrupt.estimateSeconds(input, task) == fallback);
        REQUIRE(corrupt.save());
        std::cout << "[PASS] cost model contracts\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "[FAIL] " << e.what() << '\n'; return 1;
    }
}
