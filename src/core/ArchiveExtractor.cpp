#include "ArchiveExtractor.hpp"
#include "FileUtils.hpp"
#include "ProcessManager.hpp"
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <vector>

namespace UltraArch {

namespace fs = std::filesystem;

bool ArchiveExtractor::isArchive(const fs::path &path) {
    return !detectFormat(path).empty();
}

std::string ArchiveExtractor::detectFormat(const fs::path &path) {
    if (!fs::exists(path) || !fs::is_regular_file(path))
        return "";

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return "";

    std::vector<unsigned char> magic(262); // need up to 262 bytes for tar
    file.read(reinterpret_cast<char *>(magic.data()), magic.size());
    size_t bytesRead = file.gcount();

    if (bytesRead >= 2) {
        if (magic[0] == 0x1F && magic[1] == 0x8B)
            return "gzip";
    }

    if (bytesRead >= 4) {
        if (magic[0] == 'P' && magic[1] == 'K' && magic[2] == 0x03 && magic[3] == 0x04)
            return "zip";
        if (magic[0] == 'R' && magic[1] == 'a' && magic[2] == 'r' && magic[3] == '!')
            return "rar";
    }

    if (bytesRead >= 6) {
        if (magic[0] == '7' && magic[1] == 'z' && magic[2] == 0xBC && magic[3] == 0xAF &&
            magic[4] == 0x27 && magic[5] == 0x1C)
            return "7z";
    }

    if (bytesRead >= 262) {
        if (magic[257] == 'u' && magic[258] == 's' && magic[259] == 't' && magic[260] == 'a' &&
            magic[261] == 'r')
            return "tar";
    }

    return "";
}

AnalysisSummary ArchiveExtractor::preview(const fs::path &archive, const fs::path &tools,
                                          const std::atomic<bool> *cancel,
                                          const std::string &password) {
    AnalysisSummary summary;
    FileTypeInfo current;
    bool directory = false;
    std::string error;
    auto commit = [&] {
        if (current.path.empty())
            return;
        if (!directory) {
            auto ext = pathUtf8(current.path.extension());
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".exe" || ext == ".dll") {
                current.category = FileCategory::Executable_PE;
                summary.hasExecutables = true;
            } else if (ext == ".wav" || ext == ".bmp" || ext == ".raw") {
                current.category = FileCategory::Uncompressed_Media;
                summary.hasUncompressedMedia = true;
            } else if (ext == ".txt" || ext == ".json" || ext == ".cpp" || ext == ".xml" ||
                       ext == ".csv") {
                current.category = FileCategory::Text_Database;
                summary.hasStructuredText = true;
            }
            summary.totalFiles++;
            summary.totalBytes += current.sizeBytes;
            summary.categoryBytes[current.category] += current.sizeBytes;
            summary.categoryCount[current.category]++;
            summary.files.push_back(current);
        }
        current = {};
        directory = false;
    };
    std::vector<std::string> args = {"l", "-slt", "-ba", "-sccUTF-8",
                                     pathUtf8(fs::absolute(archive))};
    if (!password.empty())
        args.push_back("-p" + password);
    ProcessControl control{nullptr, 30};
    auto r = ProcessManager::execute(
        pathUtf8(ProcessManager::resolveExecutable("7z", tools)), args, archive.parent_path(),
        nullptr, 0, cancel,
        [&](const std::string &line) {
            try {
                if (line.rfind("Path = ", 0) == 0) {
                    commit();
                    WorkspaceManager::validateArchivePath(line.substr(7));
                    current.path = utf8Path(line.substr(7));
                } else if (line.rfind("Size = ", 0) == 0)
                    current.sizeBytes = std::stoull(line.substr(7));
                else if (line == "Folder = +" || (line.rfind("Attributes = ", 0) == 0 &&
                                                  line.substr(13).find('D') != std::string::npos))
                    directory = true;
            } catch (const std::exception &e) {
                error = e.what();
            }
        },
        &control);
    commit();
    if (!r.success || !error.empty())
        throw std::runtime_error("Preview listing failed: " + error + " / " + r.errOutput);
    uint64_t max = 0;
    for (const auto &[category, bytes] : summary.categoryBytes)
        if (bytes > max) {
            summary.dominantCategory = category;
            max = bytes;
        }
    return summary;
}

bool ArchiveExtractor::extract(const fs::path &archivePath, const fs::path &outputDir,
                               const fs::path &toolsBinDir, const std::atomic<bool> *cancelToken,
                               std::function<void(const std::string &)> logCallback,
                               const std::string &password, ResourceGuard *guard,
                               uint32_t timeoutSeconds, ExtractionQuota quota) {
    auto destination = fs::absolute(outputDir);
    if (!fs::exists(archivePath)) {
        if (logCallback)
            logCallback("Archive not found: " + pathUtf8(archivePath));
        return false;
    }

    if (!fs::exists(destination)) {
        fs::create_directories(destination);
    }

    // We will use 7zz or 7z or 7za
    fs::path exePath = ProcessManager::resolveExecutable("7z", toolsBinDir);
    if (exePath.empty())
        exePath = ProcessManager::resolveExecutable("7zz", toolsBinDir);
    if (exePath.empty())
        exePath = ProcessManager::resolveExecutable("7za", toolsBinDir);

    if (exePath.empty()) {
        if (logCallback)
            logCallback("7z executable not found.");
        return false;
    }

    std::string validationError;
    uint64_t listedBytes = 0, listedCount = 0;
    std::set<std::string> names;
    std::vector<std::string> listing = {"l", "-slt", "-ba", "-sccUTF-8",
                                        pathUtf8(fs::absolute(archivePath))};
    if (!password.empty())
        listing.push_back("-p" + password);
    ProcessControl listingControl{nullptr, timeoutSeconds};
    auto listed = ProcessManager::execute(
        pathUtf8(exePath), listing, destination, guard, 0, cancelToken,
        [&](const auto &line) {
            try {
                if (line.rfind("Path = ", 0) == 0) {
                    auto name = line.substr(7);
                    WorkspaceManager::validateArchivePath(name);
                    name = archivePathKey(name);
                    if (!names.insert(name).second)
                        throw std::runtime_error("Conflicting archive names");
                    if (++listedCount > quota.files)
                        throw std::runtime_error("Archive entry quota exceeded");
                } else if (line.rfind("Size = ", 0) == 0) {
                    auto bytes = std::stoull(line.substr(7));
                    if (bytes > quota.fileBytes || bytes > quota.totalBytes - listedBytes)
                        throw std::runtime_error("Archive byte quota exceeded");
                    listedBytes += bytes;
                } else if (line.rfind("Symbolic Link = ", 0) == 0 ||
                           line.rfind("Hard Link = ", 0) == 0)
                    throw std::runtime_error("Archive links are unsupported");
            } catch (const std::exception &e) {
                validationError = e.what();
            }
        },
        &listingControl);
    if (!listed.success || !validationError.empty()) {
        if (logCallback)
            logCallback("Archive preflight failed: " + validationError + " / " + listed.errOutput);
        return false;
    }
    if (listedBytes > fs::space(destination).available) {
        if (logCallback)
            logCallback("Insufficient extraction disk space");
        return false;
    }
    std::vector<std::string> args = {"x", "-o" + pathUtf8(destination), "-y", "-mmt1",
                                     pathUtf8(fs::absolute(archivePath))};

    if (!password.empty())
        args.push_back("-p" + password);
    args.insert(args.begin() + 1, "-w" + pathUtf8(destination));
    ProcessControl control{nullptr, timeoutSeconds};
    auto lastCheck = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    control.resourceStop = [&]() {
        if (std::chrono::steady_clock::now() - lastCheck < std::chrono::milliseconds(100))
            return false;
        lastCheck = std::chrono::steady_clock::now();
        try {
            WorkspaceManager::check(destination, quota);
            return false;
        } catch (const std::exception &e) {
            validationError = e.what();
            return true;
        }
    };
    if (logCallback) {
        logCallback("Extracting archive: " + pathUtf8(archivePath.filename()));
    }

    auto result = ProcessManager::execute(pathUtf8(exePath), args,
                                          destination, // working dir
                                          guard, 0, cancelToken, logCallback, &control);

    try {
        WorkspaceManager::check(destination, quota);
    } catch (const std::exception &e) {
        validationError = e.what();
    }
    if (!result.success || !validationError.empty()) {
        if (logCallback)
            logCallback("Extraction failed: " + validationError + " / " + result.errOutput);
        return false;
    }

    if (logCallback) {
        logCallback("Extraction completed successfully.");
    }

    return true;
}

} // namespace UltraArch
