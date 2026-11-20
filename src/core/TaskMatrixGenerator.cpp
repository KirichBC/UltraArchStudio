#include "TaskMatrixGenerator.hpp"
#include "DictionaryPolicy.hpp"
#include "RamPolicy.hpp"
#include "ResourceGuard.hpp"
#include "SettingsJson.hpp"
#include <algorithm>
#include <set>
#include <thread>
namespace UltraArch {
uint64_t TaskMatrixGenerator::estimateMemoryMB(const std::vector<std::string> &flags,
                                               bool compress) {
    // Compatibility API. The optimizer itself uses typed CompressionSpec::memoryMB.
    uint64_t total = 64;
    bool found = false, store = false, globalPpmd = false;
    uint32_t mt = 1;
    for (auto flag : flags) {
        for (auto &ch : flag)
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
        globalPpmd = globalPpmd || flag.find("ppmd") != std::string::npos;
        store = store || flag == "-mx0" || flag == "-mx=0";
        if (flag.rfind("-mmt=", 0) == 0)
            mt = static_cast<uint32_t>(std::stoul(flag.substr(5)));
    }
    for (auto flag : flags) {
        for (auto &ch : flag)
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
        bool global = flag.rfind("-md", 0) == 0;
        bool ppmd = global ? globalPpmd : flag.find("ppmd") != std::string::npos;
        auto position = global ? size_t(3) : flag.find(ppmd ? ":mem" : ":d");
        if (!global && position != std::string::npos)
            position += ppmd ? 4 : 2;
        if (position == std::string::npos)
            continue;
        if (position < flag.size() && flag[position] == '=')
            ++position;
        auto begin = position;
        while (position < flag.size() && flag[position] >= '0' && flag[position] <= '9')
            ++position;
        if (position == begin)
            continue;
        uint64_t size = std::stoull(flag.substr(begin, position - begin));
        char unit = position < flag.size() ? flag[position] : 'm';
        if (unit == 'k')
            size = (size + 1023) / 1024;
        else if (unit == 'g')
            size *= 1024;
        total += ppmd ? size : (compress ? size * 12 * std::max(1u, (mt + 1) / 2) : size + 16);
        found = true;
    }
    if (store)
        return 32;
    if (!found)
        total += compress ? 32 * 12 : 48;
    return total;
}
std::vector<TaskConfig> TaskMatrixGenerator::generateMatrix(const AnalysisSummary &summary,
                                                            const OptimizerSettings &settings) {
    validateSettings(settings);
    // Workers limit concurrency; a large candidate can reserve the entire budget.
    uint64_t budget = settings.ramLimitMB;
    uint32_t cores = settings.cpuThreads
                         ? settings.cpuThreads
                         : ResourceGuard::availableCpuThreads(static_cast<int>(settings.affinityMode));
    uint32_t mt = settings.autoCpu ? std::min(2u, cores) : std::min(4u, std::max(1u, cores / settings.workerThreads));
    uint64_t dataMB = std::max<uint64_t>(1, (summary.totalBytes + 1048575) / 1048576);
    uint64_t cap = std::min<uint64_t>(
        {settings.maxLzmaDictionaryMB ? settings.maxLzmaDictionaryMB : 1536, dataMB,
         std::max<uint64_t>(1,
                            (budget + 1024 - 64) / (12 * std::max(1u, (mt + 1) / 2)))});
    std::vector<TaskConfig> tasks;
    std::set<std::string> seen;
    std::string solid = settings.solidBlockSize.empty() ? (summary.totalFiles > 1 ? "e" : "on")
                                                        : settings.solidBlockSize;
    auto lzma = [](uint64_t mb) {
        CodecSpec m;
        m.dictionary = MiB{mb}.bytes();
        return m;
    };
    auto add = [&](std::string name, std::vector<CodecSpec> methods,
                   std::vector<std::string> bindings = std::vector<std::string>{},
                   std::string block = "", bool sortByType = false) {
        TaskConfig task;
        task.taskId = static_cast<int>(tasks.size()) + 1;
        task.name = std::move(name);
        task.category = summary.dominantCategory;
        task.spec.methods = std::move(methods);
        task.spec.solid = block.empty() ? solid : block;
        DictionaryPolicy::apply(task.spec, summary, settings);
        uint64_t dictionaryKB = 0;
        for (const auto &method : task.spec.methods)
            if (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2)
                dictionaryKB = std::max(dictionaryKB, method.dictionary.value / 1024);
        auto dictionaryLabel = std::to_string(dictionaryKB) + "KiB";
        if (dictionaryKB) {
            // Names describe the effective request after block/user limits, not the
            // original generator step which may have been clamped and deduplicated.
            for (size_t i = 0; i < task.name.size();) {
                if (task.name[i] < '0' || task.name[i] > '9') { ++i; continue; }
                size_t end = i;
                while (end < task.name.size() && task.name[end] >= '0' && task.name[end] <= '9') ++end;
                if (task.name.compare(end, 2, "MB") == 0) {
                    task.name.replace(i, end + 2 - i, dictionaryLabel);
                    i += dictionaryLabel.size();
                } else i = end;
            }
        }
        if (dictionaryKB && task.name.find(dictionaryLabel) == std::string::npos)
            task.name += " [" + dictionaryLabel + "]";
        task.spec.bindings = std::move(bindings);
        task.spec.threads = task.spec.methods.front().codec == Codec::PPMd ||
                                    task.spec.methods.front().codec == Codec::Store
                                ? 1
                                : mt;
        task.spec.sortByType = sortByType || settings.sortByType;
        task.estimatedRamMB = task.spec.memoryMB();
        if (RamPolicy::eligible(settings, summary.totalBytes, summary.totalFiles + summary.emptyDirectories.size())) {
            auto reservation = RamPolicy::reservation(task, summary.totalBytes, summary.totalFiles + summary.emptyDirectories.size());
            if (reservation <= budget) {
                task.ramCandidate = true;
                task.estimatedRamMB = reservation;
            }
        }
        uint64_t requested = 0, decoding = 0;
        for (const auto &method : task.spec.methods)
            if (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2) {
                requested = std::max(requested, method.dictionary.value);
                decoding = std::max(decoding, DictionaryPolicy::decoderBytes(method));
            }
        if (requested)
            task.description = "LZMA request " + std::to_string(requested / 1024) +
                " KiB; decoder history " + std::to_string(decoding / 1024) +
                " KiB; stream bound " + std::to_string(DictionaryPolicy::streamBytes(summary, task.spec.solid)) +
                " bytes; user ceiling " + (settings.maxLzmaDictionaryMB ?
                    std::to_string(settings.maxLzmaDictionaryMB) + " MiB" : "auto");
        task.cpuTokens = task.spec.threads;
        task.configId = task.spec.id();
        task.resourceBlocked = task.estimatedRamMB > budget;
        if (task.estimatedRamMB > budget + 1024 || !seen.insert(task.configId).second)
            return;
        task.flags = task.spec.compile();
        if (!settings.password.empty()) {
            task.flags.push_back("-p" + settings.password);
            if (settings.encryptHeaders)
                task.flags.push_back("-mhe=on");
        }
        tasks.push_back(std::move(task));
    };
    CodecSpec store;
    store.codec = Codec::Store;
    add("Store (-mx0)", {store});
    std::vector<uint64_t> dicts;
    for (uint64_t d : {1ULL, 4ULL, 8ULL, 16ULL, 32ULL, 48ULL, 64ULL, 96ULL, 128ULL, 192ULL, 256ULL,
                       384ULL, 512ULL, 768ULL, 1024ULL})
        if (d <= cap)
            dicts.push_back(d);
    const auto presetDictionary = dicts.empty() ? 1 : dicts.back();
    if (settings.profile == OptimizationProfile::Extreme_RePack &&
        (dicts.empty() || dicts.back() < cap))
        dicts.push_back(cap); // Cover the complete group, including non-power-of-two data sizes.
    if (settings.profile == OptimizationProfile::WinRAR_Safe && dicts.size() > 4)
        dicts.resize(4);
    for (auto d : dicts) {
        add("Baseline LZMA2 " + std::to_string(d) + "MB", {lzma(d)});
        if (settings.solidBlockSize.empty() && summary.totalBytes > 128ULL * 1048576)
            add("LZMA2 " + std::to_string(d) + "MB Solid 128m", {lzma(d)}, {}, "128m");
        if (settings.profile != OptimizationProfile::WinRAR_Safe) {
            auto classic = lzma(d);
            classic.codec = Codec::LZMA1;
            add("LZMA1 " + std::to_string(d) + "MB", {classic});
            if (settings.solidBlockSize.empty() && summary.totalFiles > 1) {
                add("LZMA1 " + std::to_string(d) + "MB Continuous", {classic}, {}, "on");
                add("LZMA1 " + std::to_string(d) + "MB Sorted", {classic}, {}, "on", true);
                add("LZMA2 " + std::to_string(d) + "MB Continuous", {lzma(d)}, {}, "on");
            }
        }
    }
    const auto largestDictionary = dicts.empty() ? 1 : dicts.back();
    std::vector<uint64_t> specializedDictionaries{largestDictionary};
    if (presetDictionary != largestDictionary)
        specializedDictionaries.push_back(presetDictionary);
    // Extending the data-sized cap must retain previously tested context/filter specs.
    for (auto d : specializedDictionaries) {
        if (settings.profile != OptimizationProfile::WinRAR_Safe && summary.totalFiles > 1) {
            auto classic = lzma(d);
            classic.codec = Codec::LZMA1;
            add("LZMA1 Sorted full dictionary", {classic}, {},
                settings.solidBlockSize.empty() ? "on" : solid, true);
            add("LZMA2 Sorted full dictionary", {lzma(d)}, {},
                settings.solidBlockSize.empty() ? "on" : solid, true);
        }
        if (settings.profile != OptimizationProfile::WinRAR_Safe) {
            auto deep = lzma(d);
            deep.matchFinder = "bt5";
            deep.matchCycles = 100000;
            add("LZMA2 Deep bt5 mmc=100k", {deep});
            auto code = lzma(d);
            code.lc = 2;
            code.lp = 2;
            add("LZMA2 Code Context lc2 lp2", {code});
            auto x64 = lzma(d);
            x64.lc = 4;
            x64.pb = 0;
            add("LZMA2 x64 Context lc4 pb0", {x64});
            auto fast = lzma(d);
            fast.fastBytes = 64;
            add("LZMA2 fb64", {fast});
            for (auto finder : {"bt2", "bt3", "bt4", "bt5", "hc4"}) {
                auto classic = lzma(d);
                classic.codec = Codec::LZMA1;
                classic.matchFinder = finder;
                add(std::string("LZMA1 ") + finder, {classic});
            }
            for (auto context : {0u, 5u, 6u, 8u}) {
                auto classic = lzma(d);
                classic.codec = Codec::LZMA1;
                classic.lc = context;
                classic.pb = 0;
                classic.matchCycles = 200;
                add("LZMA1 context " + std::to_string(context), {classic});
            }
            if (settings.profile == OptimizationProfile::Extreme_RePack) {
                for (uint32_t lc = 0; lc <= 8; ++lc)
                    for (uint32_t pb : {0u, 2u, 4u}) {
                        auto classic = lzma(d);
                        classic.codec = Codec::LZMA1;
                        classic.lc = lc;
                        classic.pb = pb;
                        add("LZMA1 lc" + std::to_string(lc) + " pb" + std::to_string(pb), {classic});
                    }
                for (uint32_t lp = 1; lp <= 4; ++lp) {
                    auto classic = lzma(d);
                    classic.codec = Codec::LZMA1;
                    classic.lc = 0;
                    classic.lp = lp;
                    classic.pb = 0;
                    add("LZMA1 lc0 lp" + std::to_string(lp) + " pb0", {classic});
                }
            }
            for (auto codec : {Codec::BZip2, Codec::Deflate}) {
                CodecSpec extra;
                extra.codec = codec;
                add(codec == Codec::BZip2 ? "BZip2" : "Deflate", {extra});
            }
        }
        if (summary.hasExecutables) {
            CodecSpec bcj;
            bcj.codec = Codec::BCJ;
            add("BCJ + LZMA2", {bcj, lzma(d)});
            CodecSpec bcj2;
            bcj2.codec = Codec::BCJ2;
            auto side = lzma(1);
            side.fastBytes = 32;
            add("BCJ2 4-Stream", {bcj2, lzma(std::min<uint64_t>(d, 8)), lzma(1), lzma(1), side},
                {"0:1", "0s1:2", "0s2:3", "0s3:4"});
            if (settings.profile != OptimizationProfile::WinRAR_Safe)
                for (auto codec : {Codec::LZMA1, Codec::LZMA2}) {
                    auto main = lzma(d);
                    main.codec = codec;
                    add(codec == Codec::LZMA1 ? "BCJ + LZMA1" : "BCJ + LZMA2 Continuous",
                        {bcj, main}, {}, settings.solidBlockSize.empty() ? "on" : solid);
                    for (auto dictionary : {std::min<uint64_t>(d, 64), std::min<uint64_t>(d, 256), d}) {
                        main.dictionary = MiB{dictionary}.bytes();
                        auto branch = lzma(1);
                        branch.codec = Codec::LZMA1;
                        branch.lc = 0;
                        branch.lp = 2;
                        add(std::string("BCJ2 ") + (codec == Codec::LZMA1 ? "LZMA1 " : "LZMA2 ") +
                                std::to_string(dictionary) + "MB",
                            {bcj2, main, branch, branch}, {"0:1", "0s1:2", "0s2:3"},
                            settings.solidBlockSize.empty() ? "on" : solid);
                    }
                    if (codec == Codec::LZMA2) {
                        auto branch = lzma(1);
                        branch.codec = Codec::LZMA1;
                        branch.lc = 0;
                        branch.lp = 2;
                        CodecSpec delta;
                        delta.codec = Codec::Delta;
                        delta.delta = 4;
                        auto fourth = lzma(1);
                        fourth.lc = 4;
                        fourth.pb = 0;
                        add("BCJ2 Delta-4 four-stream", {bcj2, main, branch, delta, branch, fourth},
                            {"0s0:1", "0s1:2", "0s2:3", "0s3:5", "3s0:4"},
                            settings.solidBlockSize.empty() ? "on" : solid);
                        add("BCJ2 Delta-4 four-stream Sorted", {bcj2, main, branch, delta, branch, fourth},
                            {"0s0:1", "0s1:2", "0s2:3", "0s3:5", "3s0:4"},
                            settings.solidBlockSize.empty() ? "on" : solid, true);
                        main.lc = 4;
                        branch.fastBytes = 64;
                        fourth.fastBytes = 64;
                        add("BCJ2 lc4 Delta-4 fb64 branches", {bcj2, main, branch, delta, branch, fourth},
                            {"0s0:1", "0s1:2", "0s2:3", "0s3:5", "3s0:4"},
                            settings.solidBlockSize.empty() ? "on" : solid);
                        add("BCJ2 lc4 Delta-4 fb64 branches Sorted", {bcj2, main, branch, delta, branch, fourth},
                            {"0s0:1", "0s1:2", "0s2:3", "0s3:5", "3s0:4"},
                            settings.solidBlockSize.empty() ? "on" : solid, true);
                    }
                }
        }
        if (summary.hasStructuredText || settings.profile != OptimizationProfile::WinRAR_Safe) {
            for (uint64_t mem : {16ULL, 64ULL, 128ULL, 256ULL, 512ULL, 1024ULL, 1536ULL}) {
                if (mem > 256 && (settings.profile == OptimizationProfile::WinRAR_Safe ||
                                 mem > std::max<uint64_t>(64, dataMB * 2)))
                    continue;
                CodecSpec ppmd;
                ppmd.codec = Codec::PPMd;
                ppmd.dictionary = MiB{mem}.bytes();
                add("PPMd Order-8 " + std::to_string(mem) + "MB", {ppmd});
                if (settings.profile != OptimizationProfile::WinRAR_Safe) {
                    for (auto order : {10u, 12u, 16u, 20u, 24u, 32u}) {
                        ppmd.order = order;
                        add("PPMd Order-" + std::to_string(order) + " " + std::to_string(mem) + "MB", {ppmd});
                    }
                }
            }
        }
        if (summary.hasUncompressedMedia ||
            (settings.profile == OptimizationProfile::Extreme_RePack && summary.totalBytes <= 80ULL * 1048576)) {
            for (uint32_t stride = 1; stride <= 32; ++stride) {
                if (settings.profile == OptimizationProfile::WinRAR_Safe && stride > 8)
                    continue;
                CodecSpec delta;
                delta.codec = Codec::Delta;
                delta.delta = stride;
                add("Delta " + std::to_string(stride), {delta, lzma(d)});
                if (settings.profile == OptimizationProfile::Extreme_RePack) {
                    auto classic = lzma(d);
                    classic.codec = Codec::LZMA1;
                    add("Delta LZMA1 " + std::to_string(stride), {delta, classic});
                    classic.lc = 0;
                    classic.lp = 1;
                    add("Delta LZMA1 lc0 lp1 " + std::to_string(stride), {delta, classic});
                    classic.lp = 0;
                    classic.lc = 7;
                    classic.pb = 4;
                    add("Delta LZMA1 lc7 pb4 " + std::to_string(stride), {delta, classic});
                    classic.lc = 8;
                    classic.pb = 0;
                    add("Delta LZMA1 lc8 pb0 " + std::to_string(stride), {delta, classic});
                }
            }
        }
    }
    return tasks;
}
} // namespace UltraArch
