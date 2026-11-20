#pragma once
#include "Hasher.hpp"
#include <array>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace UltraArch {
inline std::string pathUtf8(const std::filesystem::path &p) {
    auto s = p.u8string();
    return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}
inline std::filesystem::path utf8Path(const std::string &s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
// Callers validate archive entry names before joining. Archive manifests retain
// '/' separators, but extended Windows paths bypass Win32 slash conversion.
inline std::filesystem::path archiveEntryPath(const std::filesystem::path &root,
                                               const std::string &relative) {
    auto path = root / utf8Path(relative);
    return path.make_preferred();
}
inline std::string archivePathKey(std::string path) {
    // Empty extension is a valid grouping key; actual archive names are validated separately.
    if (path.empty())
        return {};
    for (char &ch : path)
        if (ch == '\\')
            ch = '/';
#ifdef _WIN32
    auto wide = utf8Path(path).wstring();
    int length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(),
                               static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
    if (length <= 0)
        throw std::runtime_error("Cannot normalize archive name");
    std::wstring mapped(static_cast<size_t>(length), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(),
                       static_cast<int>(wide.size()), mapped.data(), length, nullptr, nullptr, 0))
        throw std::runtime_error("Cannot normalize archive name");
    return pathUtf8(std::filesystem::path(mapped));
#else
    for (char &ch : path)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return path;
#endif
}
inline bool isLinkOrReparse(const std::filesystem::path &path) {
#ifdef _WIN32
    auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return std::filesystem::is_symlink(path);
#endif
}
inline std::string uniqueId() {
    std::random_device rd;
    std::ostringstream out;
    out << std::hex << rd() << rd() << rd() << rd();
    return out.str();
}
inline bool isWithin(const std::filesystem::path &child, const std::filesystem::path &parent) {
    auto c = std::filesystem::weakly_canonical(child);
    auto p = std::filesystem::weakly_canonical(parent);
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci) {
        if (ci == c.end())
            return false;
#ifdef _WIN32
        const auto &left = ci->native();
        const auto &right = pi->native();
        if (CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                 static_cast<int>(right.size()), TRUE) != CSTR_EQUAL)
            return false;
#else
        if (*ci != *pi)
            return false;
#endif
    }
    return true;
}
inline bool atomicReplace(const std::filesystem::path &from, const std::filesystem::path &to,
                          std::error_code &ec) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    ec.clear();
#else
    std::filesystem::rename(from, to, ec);
#endif
    return !ec;
}
inline void syncFile(const std::filesystem::path &path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot flush file");
    bool ok = FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
#else
    int handle = ::open(path.c_str(), O_RDONLY);
    if (handle < 0)
        throw std::runtime_error("Cannot flush file");
    bool ok = ::fsync(handle) == 0;
    ::close(handle);
#endif
    if (!ok)
        throw std::runtime_error("Cannot persist file");
}
inline bool writeAtomically(const std::filesystem::path &path, const std::string &data) {
    auto tmp = path.parent_path() / utf8Path(pathUtf8(path.filename()) + "." + uniqueId() + ".tmp");
    std::ofstream out(tmp, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    std::error_code ec;
    bool ok = false;
    try {
        if (out) {
            syncFile(tmp);
            ok = atomicReplace(tmp, path, ec);
        }
    } catch (...) {
        ok = false;
    }
#ifndef _WIN32
    if (ok) {
        int dir = ::open(path.parent_path().c_str(), O_RDONLY);
        if (dir >= 0) {
            ::fsync(dir);
            ::close(dir);
        }
    }
#endif
    if (!ok)
        std::filesystem::remove(tmp, ec);
    return ok;
}
inline std::string fileChecksum(const std::filesystem::path &path,
                                const std::atomic<bool> *cancel = nullptr) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot read file: " + pathUtf8(path));
    Hasher hash;
    std::array<char, 262144> buffer{};
    while (in.read(buffer.data(), buffer.size()) || in.gcount()) {
        if (cancel && cancel->load())
            throw std::runtime_error("Cancelled");
        hash.update(buffer.data(), static_cast<size_t>(in.gcount()));
    }
    if (!in.eof())
        throw std::runtime_error("Read failed: " + pathUtf8(path));
    return hash.finish();
}
inline std::string redactFlags(const std::vector<std::string> &flags) {
    std::string result;
    for (const auto &f : flags) {
        if (!result.empty())
            result += ' ';
        result += f.rfind("-p", 0) == 0 ? "-p<redacted>" : f;
    }
    return result;
}
inline std::string quoteWindowsArgument(const std::string &arg) {
    std::string result = "\"";
    size_t slashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        if (c == '"')
            result.append(slashes * 2 + 1, '\\');
        else
            result.append(slashes, '\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, '\\');
    result += '"';
    return result;
}
} // namespace UltraArch
