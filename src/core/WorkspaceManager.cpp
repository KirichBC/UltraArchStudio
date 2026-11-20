#include "WorkspaceManager.hpp"
#include <algorithm>
namespace UltraArch {
void WorkspaceManager::validateArchivePath(const std::string &path) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    auto p = utf8Path(normalized);
    if (std::any_of(path.begin(), path.end(), [](unsigned char ch) { return ch < 32; }))
        throw std::runtime_error("Control character in archive path");
    if (path.empty() || path.find_first_of("\r\n:") != std::string::npos || p.is_absolute() ||
        p.has_root_name() || p.has_root_directory())
        throw std::runtime_error("Unsafe archive path: " + path);
    for (const auto &component : p) {
        auto part = pathUtf8(component);
        auto stem = part.substr(0, part.find('.'));
        for (auto &ch : stem)
            if (ch >= 'a' && ch <= 'z')
                ch = static_cast<char>(ch - 'a' + 'A');
        bool reserved =
            stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
            (stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) &&
             stem[3] >= '1' && stem[3] <= '9');
        if (part == ".." || part == "." || part.empty() || part.back() == '.' ||
            part.back() == ' ' || part.find_first_of("<>\"|?*") != std::string::npos || reserved)
            throw std::runtime_error("Unsupported archive path: " + path);
    }
}
uint64_t WorkspaceManager::check(const std::filesystem::path &root, const ExtractionQuota &quota) {
    uint64_t bytes = 0, count = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_symlink())
            throw std::runtime_error("Extraction created a symlink");
#ifdef _WIN32
        auto attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("Extraction created a reparse point");
#endif
        validateArchivePath(pathUtf8(entry.path().lexically_relative(root)));
        if (++count > quota.files)
            throw std::runtime_error("Extraction entry count quota exceeded");
        if (entry.is_regular_file()) {
            auto size = entry.file_size();
            if (size > quota.fileBytes || size > quota.totalBytes - bytes)
                throw std::runtime_error("Extraction byte quota exceeded");
            bytes += size;
        }
    }
    auto available = std::filesystem::space(root).available;
    if (available < 16ULL * 1048576)
        throw std::runtime_error("Scratch disk is nearly full");
    return bytes;
}
void WorkspaceManager::mark(const std::filesystem::path &workspace, const std::string &id) {
    if (!writeAtomically(workspace / ".ultraarch-owner", id))
        throw std::runtime_error("Cannot mark workspace ownership");
}
bool WorkspaceManager::owned(const std::filesystem::path &workspace,
                             const std::filesystem::path &base) {
    if (workspace.parent_path() != std::filesystem::absolute(base) || isLinkOrReparse(workspace))
        return false;
    auto name = pathUtf8(workspace.filename());
    if (name.rfind("run_", 0) != 0 ||
        !std::filesystem::is_regular_file(workspace / ".ultraarch-owner"))
        return false;
    auto marker = workspace / ".ultraarch-owner";
    std::error_code ec;
    auto size = std::filesystem::file_size(marker, ec);
    if (ec || size == 0 || size > 128 || isLinkOrReparse(marker))
        return false;
    std::ifstream in(marker);
    std::string id;
    in >> id;
    return name == "run_" + id;
}
} // namespace UltraArch
