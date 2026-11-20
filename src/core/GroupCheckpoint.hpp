#pragma once
#include "FileUtils.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace UltraArch {
// Immutable archive generations make the small atomic marker the commit point.
// An interrupted replacement never changes the bytes named by the old marker.
struct GroupCheckpoint {
    using Json = nlohmann::json;
    static std::filesystem::path artifact(const std::filesystem::path &directory, const Json &record) {
        const auto name = record.value("artifact", std::string{"winner.7z"});
        const bool generation = name.size() == 42 && name.rfind("winner-", 0) == 0 &&
            name.substr(39) == ".7z" && std::all_of(name.begin() + 7, name.begin() + 39,
                [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        if (name != "winner.7z" && !generation)
            throw std::runtime_error("Unsafe group checkpoint artifact name");
        const auto path = directory / utf8Path(name);
        if (isLinkOrReparse(path)) throw std::runtime_error("Group checkpoint artifact is a link");
        return path;
    }
    static Json read(const std::filesystem::path &directory) {
        const auto marker = directory / "winner.json";
        if (isLinkOrReparse(marker)) throw std::runtime_error("Group checkpoint marker is a link");
        std::ifstream input(marker, std::ios::binary);
        Json record; input >> record;
        artifact(directory, record);
        return record;
    }
    static std::filesystem::path publish(const std::filesystem::path &directory,
        const std::filesystem::path &source, Json record,
        const std::function<void()> &beforeCommit = {}) {
        Json previous = Json::object();
        try { previous = read(directory); } catch (...) {}
        const auto sourceHash = fileChecksum(source);
        const bool reuse = previous.is_object() && previous.value("sha256", "") == sourceHash &&
            previous.value("configId", "") == record.value("configId", "") &&
            std::filesystem::is_regular_file(artifact(directory, previous)) &&
            fileChecksum(artifact(directory, previous)) == sourceHash;
        const auto name = reuse ? previous.value("artifact", std::string{"winner.7z"}) :
            "winner-" + Hasher::text(uniqueId()).substr(0, 32) + ".7z";
        const auto target = directory / utf8Path(name);
        if (!reuse) {
            std::filesystem::copy_file(source, target);
            syncFile(target);
        }
        record["artifact"] = name;
        record["sha256"] = sourceHash;
        if (!record.value("contentVerified", false) && previous.is_object()) {
            if (previous.value("contentVerified", false)) {
                previous.erase("contentFallback");
                record["contentFallback"] = previous;
            } else if (previous.contains("contentFallback"))
                record["contentFallback"] = previous.at("contentFallback");
        }
        if (beforeCommit) beforeCommit();
        if (!writeAtomically(directory / "winner.json", record.dump(2)))
            throw std::runtime_error("Cannot commit durable group checkpoint");
        // Reclaim only archives referenced by the committed predecessor; leave
        // interrupted/unrecognized generations for explicit session cleanup.
        for (const auto &old : {previous, previous.value("contentFallback", Json{})}) {
            if (!old.is_object()) continue;
            const auto obsolete = artifact(directory, old);
            const auto keep = record.contains("contentFallback") ? artifact(directory, record.at("contentFallback")) : target;
            if (obsolete != target && obsolete != keep) {
                std::error_code ec; std::filesystem::remove(obsolete, ec);
            }
        }
        return target;
    }
    static void copyTo(const std::filesystem::path &source, const std::filesystem::path &destination) {
        const auto record = read(source);
        std::filesystem::create_directories(destination);
        for (const auto &item : {record, record.value("contentFallback", Json{})}) {
            if (!item.is_object()) continue;
            const auto from = artifact(source, item);
            if (fileChecksum(from) != item.at("sha256").get<std::string>())
                throw std::runtime_error("Group checkpoint archive identity mismatch");
            const auto to = destination / from.filename();
            std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing);
            syncFile(to);
        }
        if (!writeAtomically(destination / "winner.json", record.dump(2)))
            throw std::runtime_error("Cannot copy durable group checkpoint marker");
    }
};
} // namespace UltraArch
