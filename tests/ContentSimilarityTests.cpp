#include "core/ContentSimilarity.hpp"
#include <iostream>
#include <set>
#include <stdexcept>
using namespace UltraArch;
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
namespace {
FileTypeInfo file(const std::string &name, unsigned family, FileCategory category = FileCategory::Generic_Binary) {
    FileTypeInfo result; result.path = name; result.sizeBytes = 1048576; result.category = category;
    if (family) result.similaritySketch = {family, family + 10ULL, family + 20ULL, family + 30ULL};
    return result;
}
std::map<std::string, std::pair<uint64_t, std::string>> contents(const ContentSimilarity::Groups &groups) {
    std::map<std::string, std::pair<uint64_t, std::string>> found;
    for (const auto &[key, files] : groups)
        for (const auto &file : files) {
            const auto name = pathUtf8(file.path);
            REQUIRE(found.emplace(name, std::make_pair(file.sizeBytes, file.contentChecksum)).second);
        }
    return found;
}
void sampleAndPartitions() {
    std::vector<uint8_t> sample(4096);
    for (size_t i = 0; i < sample.size(); ++i) sample[i] = static_cast<uint8_t>(i * 37 + i / 512);
    std::array<uint64_t, 4> a{}, b{};
    ContentSimilarity::addSample(a, sample.data(), sample.size());
    ContentSimilarity::addSample(b, sample.data(), 2048);
    ContentSimilarity::addSample(b, sample.data() + 2048, 2048);
    REQUIRE(a == b);
    REQUIRE(std::all_of(a.begin(), a.end(), [](auto hash) { return hash != 0; }));
    ContentSimilarity::addSample(b, nullptr, 0); REQUIRE(a == b);
    ContentSimilarity::Groups groups{{"binary", {file("left/a.bin", 1), file("right/b.bin", 1),
                                                   file("left/c.bin", 2), file("right/d.bin", 2), file("left.bin", 0)}}};
    auto partitioned = ContentSimilarity::partition(groups);
    REQUIRE(partitioned.size() == 3);
    REQUIRE(contents(groups) == contents(partitioned));
    auto reversed = groups; std::reverse(reversed["binary"].begin(), reversed["binary"].end());
    auto again = ContentSimilarity::partition(reversed);
    std::set<std::string> keys, repeated;
    for (const auto &[key, value] : partitioned) keys.insert(key);
    for (const auto &[key, value] : again) repeated.insert(key);
    REQUIRE(keys == repeated && contents(again) == contents(groups));
    // Extraction/session roots may move; membership identities use relative paths,
    // while returned files retain their exact physical source paths.
    auto firstLocation = groups, relocated = groups;
    for (auto &f : firstLocation["binary"]) f.path = std::filesystem::path("C:/old-session/input") / f.path;
    for (auto &f : relocated["binary"]) f.path = std::filesystem::path("D:/new-session/input") / f.path;
    auto firstPartitions = ContentSimilarity::partition(firstLocation);
    auto relocatedPartitions = ContentSimilarity::partition(relocated);
    std::set<std::string> firstKeys, relocatedKeys;
    for (const auto &[key, value] : firstPartitions) firstKeys.insert(key);
    for (const auto &[key, value] : relocatedPartitions) relocatedKeys.insert(key);
    REQUIRE(firstKeys == relocatedKeys && firstKeys == keys);
    REQUIRE(contents(firstPartitions) == contents(firstLocation));
    REQUIRE(contents(relocatedPartitions) == contents(relocated));
    // Two matching minima suffice; the two other seeds may differ.
    groups["binary"][1].similaritySketch[2] = 500;
    groups["binary"][1].similaritySketch[3] = 600;
    REQUIRE(ContentSimilarity::partition(groups).size() == 3);
}
void fallbackAndExactDuplicates() {
    ContentSimilarity::Groups missing{{"text", {file("a.txt", 0, FileCategory::Text_Database),
        file("b.txt", 0, FileCategory::Text_Database), file("c.txt", 0, FileCategory::Text_Database),
        file("d.txt", 0, FileCategory::Text_Database)}}};
    REQUIRE(ContentSimilarity::partition(missing).count("text") == 1);
    missing["text"][0].contentChecksum = missing["text"][1].contentChecksum = std::string(64, 'a');
    missing["text"][2].contentChecksum = missing["text"][3].contentChecksum = std::string(64, 'b');
    auto split = ContentSimilarity::partition(missing);
    REQUIRE(split.size() == 2 && contents(split) == contents(missing));
    for (auto category : {FileCategory::Executable_PE, FileCategory::Precompressed_HighEntropy,
                          FileCategory::Uncompressed_Media}) {
        auto protectedGroups = missing;
        for (auto &f : protectedGroups["text"]) f.category = category;
        REQUIRE(ContentSimilarity::partition(protectedGroups).count("text") == 1);
    }
    auto small = missing; for (auto &f : small["text"]) f.sizeBytes = 65536;
    REQUIRE(ContentSimilarity::partition(small).count("text") == 1);
    ContentSimilarity::Groups many;
    for (unsigned n = 0; n < 20; ++n)
        many["many"].push_back(file(std::to_string(n) + ".bin", n / 2 + 1));
    auto bounded = ContentSimilarity::partition(many);
    REQUIRE(bounded.size() <= 8 && contents(bounded) == contents(many));
}
}
int main() {
    try { sampleAndPartitions(); fallbackAndExactDuplicates();
        std::cout << "[PASS] content similarity contracts\n"; return 0;
    } catch (const std::exception &e) { std::cerr << "[FAIL] " << e.what() << '\n'; return 1; }
}
