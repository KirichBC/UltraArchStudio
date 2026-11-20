#include "PublicationTransaction.hpp"
#include <algorithm>
#include <fstream>
#ifndef _WIN32
#include <sys/file.h>
#endif
namespace UltraArch {
namespace fs = std::filesystem;
namespace {
bool volume(const std::string &name, const std::string &base) {
    if (name.rfind(base + ".", 0) != 0)
        return false;
    auto suffix = name.substr(base.size() + 1);
    return suffix.size() >= 3 && suffix.find_first_not_of("0123456789") == std::string::npos;
}
void crashPoint(const std::string &phase) {
#ifdef ULTRAARCH_FAULT_INJECTION
    auto requested = std::getenv("ULTRAARCH_CRASH_PHASE");
    if (requested && phase == requested)
        std::_Exit(86);
#else
    (void)phase;
#endif
}
nlohmann::json load(const fs::path &stage) {
    auto path = stage / "journal.json";
    if (isLinkOrReparse(stage) || isLinkOrReparse(path) || fs::file_size(path) > 4 * 1048576)
        throw std::runtime_error("Unsafe publication journal");
    for (const auto &directory : {stage / "new", stage / "backup"})
        if (!fs::is_directory(directory) || isLinkOrReparse(directory))
            throw std::runtime_error("Unsafe publication directory");
    nlohmann::json j;
    std::ifstream in(path);
    in >> j;
    if (j.at("owner") != "UltraArchStudio" || j.at("schema") != 1 ||
        j.at("id").get<std::string>() != ".ultraarch-output-" + j.at("nonce").get<std::string>() ||
        j.at("id").get<std::string>() != pathUtf8(stage.filename()))
        throw std::runtime_error("Publication owner mismatch");
    auto target = utf8Path(j.at("target").get<std::string>());
    if (target.parent_path() != fs::absolute(stage).parent_path())
        throw std::runtime_error("Journal target mismatch");
    for (const auto &item : j.at("items")) {
        auto name = item.at("name").get<std::string>();
        if (utf8Path(name).filename() != utf8Path(name) || name == "." || name == ".." ||
            (!volume(name, pathUtf8(target.filename())) && name != pathUtf8(target.filename())))
            throw std::runtime_error("Unsafe journal filename");
    }
    return j;
}
} // namespace
struct PublicationTransaction::Lock {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit Lock(const fs::path &parent) {
        auto path = parent / ".ultraarch-publication.lock";
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Output folder is locked by another publication/recovery");
    }
    ~Lock() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
#else
    int handle = -1;
    explicit Lock(const fs::path &parent) {
        auto path = parent / ".ultraarch-publication.lock";
        handle = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
        if (handle < 0 || flock(handle, LOCK_EX | LOCK_NB) != 0) {
            if (handle >= 0)
                ::close(handle);
            throw std::runtime_error("Output folder is locked by another publication/recovery");
        }
    }
    ~Lock() {
        if (handle >= 0)
            ::close(handle);
    }
#endif
};
PublicationTransaction::PublicationTransaction(fs::path target)
    : target_(fs::absolute(target).lexically_normal()) {
    fs::create_directories(target_.parent_path());
    lock_ = std::make_unique<Lock>(target_.parent_path());
    if (!pending(target_.parent_path()).empty())
        throw std::runtime_error("Pending publication must be recovered first");
    auto nonce = uniqueId();
    stage_ = target_.parent_path() / (".ultraarch-output-" + nonce);
    fs::create_directories(stage_ / "new");
    fs::create_directories(stage_ / "backup");
    journal_ = {{"schema", 1},
                {"owner", "UltraArchStudio"},
                {"nonce", nonce},
                {"id", pathUtf8(stage_.filename())},
                {"target", pathUtf8(target_)},
                {"phase", "Prepared"},
                {"items", nlohmann::json::array()}};
    save("Prepared");
}
PublicationTransaction::~PublicationTransaction() {
    if (!touched_ || committed_) {
        std::error_code ec;
        fs::remove_all(stage_, ec);
    }
    // Once publication starts only an explicit verified rollback may discard the journal.
}
void PublicationTransaction::save(const std::string &phase) {
    journal_["phase"] = phase;
    if (!writeAtomically(stage_ / "journal.json", journal_.dump(2)))
        throw std::runtime_error("Cannot persist publication intent");
    crashPoint(phase);
}
void PublicationTransaction::verified(const std::vector<fs::path> &files) {
    std::map<std::string, nlohmann::json> items;
    for (const auto &file : files) {
        if (file.parent_path() != stage_ / "new")
            throw std::runtime_error("Output is outside publication workspace");
        auto name = pathUtf8(file.filename());
        syncFile(file);
        items[name] = {{"name", name}, {"newHash", fileChecksum(file)}, {"oldHash", ""}};
    }
    auto base = pathUtf8(target_.filename());
    for (const auto &entry : fs::directory_iterator(target_.parent_path())) {
        auto name = pathUtf8(entry.path().filename());
        if (name != base && !volume(name, base))
            continue;
        if (!entry.is_regular_file() || entry.is_symlink())
            throw std::runtime_error("Cannot replace non-regular output");
        if (!items.count(name))
            items[name] = {{"name", name}, {"newHash", ""}, {"oldHash", ""}};
        items[name]["oldHash"] = fileChecksum(entry.path());
    }
    for (auto &[name, item] : items)
        journal_["items"].push_back(item);
    save("Verified");
}
void PublicationTransaction::publish(const std::atomic<bool> *cancel) {
    touched_ = true;
    save("BackingUp");
    for (const auto &item : journal_["items"]) {
        if (cancel && cancel->load())
            throw std::runtime_error("Publication cancelled");
        auto name = utf8Path(item["name"].get<std::string>());
        auto oldHash = item["oldHash"].get<std::string>();
        if (oldHash.empty())
            continue;
        auto original = target_.parent_path() / name;
        if (fileChecksum(original) != oldHash)
            throw std::runtime_error("Previous output changed during publication");
        auto backup = stage_ / "backup" / name;
        fs::copy_file(original, backup);
        syncFile(backup);
        // The intent already contains both old and new identities before modifying the destination.
    }
    save("Publishing");
    for (const auto &item : journal_["items"]) {
        if (cancel && cancel->load())
            throw std::runtime_error("Publication cancelled");
        auto name = utf8Path(item["name"].get<std::string>());
        auto dest = target_.parent_path() / name;
        auto oldHash = item["oldHash"].get<std::string>();
        if (fs::exists(dest) && (oldHash.empty() || fileChecksum(dest) != oldHash))
            throw std::runtime_error("Output changed before replace");
        if (item["newHash"].get<std::string>().empty())
            fs::remove(dest);
        else {
            std::error_code ec;
            if (!atomicReplace(stage_ / "new" / name, dest, ec))
                throw fs::filesystem_error("Cannot publish", dest, ec);
        }
        crashPoint("AfterFile");
    }
    save("Committed");
    committed_ = true;
}
std::vector<fs::path> PublicationTransaction::outputs() const {
    std::vector<fs::path> outputs;
    for (const auto &item : journal_["items"])
        if (!item["newHash"].get<std::string>().empty())
            outputs.push_back(target_.parent_path() / utf8Path(item["name"].get<std::string>()));
    return outputs;
}
void PublicationTransaction::rollback() {
    recoverImpl(stage_);
    touched_ = false;
}
std::vector<fs::path> PublicationTransaction::pending(const fs::path &directory) {
    std::vector<fs::path> result;
    if (!fs::is_directory(directory))
        return result;
    for (const auto &entry : fs::directory_iterator(directory)) {
        if (pathUtf8(entry.path().filename()).rfind(".ultraarch-output-", 0) != 0)
            continue;
        try {
            load(entry.path());
            result.push_back(entry.path());
        } catch (...) {
        } // Unknown directories are never removed.
    }
    return result;
}
void PublicationTransaction::recover(const fs::path &stage) {
    Lock lock(fs::absolute(stage).parent_path());
    recoverImpl(stage);
}
void PublicationTransaction::recoverImpl(const fs::path &stage) {
    auto journal = load(stage);
    auto parent = fs::absolute(stage).parent_path();
    auto phase = journal.at("phase").get<std::string>();
    if (phase == "Committed" || phase == "Prepared" || phase == "Verified") {
        fs::remove_all(stage);
        return;
    }
    if (phase != "BackingUp" && phase != "Publishing")
        throw std::runtime_error("Unknown publication phase");
    // Validate all identities before touching any destination.
    for (const auto &item : journal["items"]) {
        auto name = utf8Path(item["name"].get<std::string>());
        auto dest = parent / name, backup = stage / "backup" / name;
        auto old = item["oldHash"].get<std::string>(), fresh = item["newHash"].get<std::string>();
        if (fs::exists(dest)) {
            if (isLinkOrReparse(dest) || !fs::is_regular_file(dest))
                throw std::runtime_error("Recovery destination unsafe");
            auto hash = fileChecksum(dest);
            if (hash != old && hash != fresh)
                throw std::runtime_error(
                    "Recovery refused: output modified by another application");
        }
        bool oldIntact = fs::exists(dest) && !old.empty() && fileChecksum(dest) == old;
        if (!old.empty() && !oldIntact &&
            (!fs::is_regular_file(backup) || isLinkOrReparse(backup) ||
             fileChecksum(backup) != old))
            throw std::runtime_error("Recovery refused: valid previous output backup missing");
    }
    for (const auto &item : journal["items"]) {
        auto name = utf8Path(item["name"].get<std::string>());
        auto dest = parent / name, backup = stage / "backup" / name;
        auto old = item["oldHash"].get<std::string>();
        if (old.empty()) {
            if (fs::exists(dest))
                fs::remove(dest);
        } else if (!fs::exists(dest) || fileChecksum(dest) != old) {
            // Keep the backup until every restoration has succeeded (recovery itself may crash).
            auto temp = stage / "new" / (pathUtf8(name) + ".restore");
            fs::copy_file(backup, temp, fs::copy_options::overwrite_existing);
            syncFile(temp);
            std::error_code ec;
            if (!atomicReplace(temp, dest, ec))
                throw fs::filesystem_error("Recovery failed; backup retained", dest, ec);
        }
    }
    fs::remove_all(stage);
}
} // namespace UltraArch
