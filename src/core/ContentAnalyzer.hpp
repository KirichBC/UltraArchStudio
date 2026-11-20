#pragma once

#include "Types.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <vector>

namespace UltraArch {

struct AnalysisSummary {
    uint64_t totalFiles = 0;
    uint64_t totalBytes = 0;
    std::map<FileCategory, uint64_t> categoryBytes;
    std::map<FileCategory, uint64_t> categoryCount;
    FileCategory dominantCategory = FileCategory::Generic_Binary;
    double averageEntropy = 0.0;
    bool hasExecutables = false;
    bool hasUncompressedMedia = false;
    bool hasStructuredText = false;
    bool hasDeflateStreams = false;
    std::vector<FileTypeInfo> files;
    std::vector<std::filesystem::path> emptyDirectories;
};

struct CachedFileInfo {
    uint64_t sizeBytes;
    std::filesystem::file_time_type mtime;
    FileCategory category;
    double entropy;
    std::string detectedType;
};

class ContentAnalyzer {
  public:
    ContentAnalyzer() = default;

    // Analyzes a single file or recursively scans a directory
    AnalysisSummary analyze(const std::filesystem::path &targetPath, bool hashContents = false,
                            const std::atomic<bool> *cancel = nullptr,
                            std::function<void(uint64_t, uint64_t)> progress = {},
                            bool similarity = false);

    // Fast magic bytes and entropy inspector for a single file
    static FileTypeInfo inspectFile(const std::filesystem::path &filePath,
                                    bool hashContents = false,
                                    const std::atomic<bool> *cancel = nullptr, bool similarity = false);
    static FileTypeInfo inspectMemory(const std::filesystem::path &logicalPath,
                                     std::shared_ptr<const MemoryContent> source,
                                     const std::atomic<bool> *cancel = nullptr, bool similarity = false);

    // Calculates Shannon entropy of a buffer (0.0 to 8.0)
    static double calculateEntropy(const uint8_t *data, size_t length);

  private:
    static FileCategory detectCategory(const uint8_t *header, size_t bytesRead,
                                       const std::filesystem::path &path, double entropy,
                                       std::string &outTypeName);
};

} // namespace UltraArch
