#include "ContentAnalyzer.hpp"
#include "ContentSimilarity.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>

namespace UltraArch {

static std::string toLower(std::string_view str) {
    std::string s(str);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

double ContentAnalyzer::calculateEntropy(const uint8_t *data, size_t length) {
    if (!data || length == 0)
        return 0.0;

    std::array<size_t, 256> freq{};
    for (size_t i = 0; i < length; ++i) {
        freq[data[i]]++;
    }

    double entropy = 0.0;
    const double len = static_cast<double>(length);
    for (size_t f : freq) {
        if (f > 0) {
            double p = static_cast<double>(f) / len;
            entropy -= p * std::log2(p);
        }
    }
    return entropy;
}

FileCategory ContentAnalyzer::detectCategory(const uint8_t *header, size_t bytesRead,
                                             const std::filesystem::path &path, double entropy,
                                             std::string &outTypeName) {
    std::string ext = toLower(pathUtf8(path.extension()));

    // 1. PE32 / PE64 detection (MZ Header + PE Signature)
    if (bytesRead >= 64 && header[0] == 'M' && header[1] == 'Z') {
        uint32_t peOffset = uint32_t(header[0x3C]) | (uint32_t(header[0x3D]) << 8) |
                            (uint32_t(header[0x3E]) << 16) | (uint32_t(header[0x3F]) << 24);
        if (size_t(peOffset) <= bytesRead - 4 && header[peOffset] == 'P' &&
            header[peOffset + 1] == 'E' && header[peOffset + 2] == 0 && header[peOffset + 3] == 0) {
            if (size_t(peOffset) <= bytesRead - 6) {
                uint16_t machine =
                    uint16_t(header[peOffset + 4]) | (uint16_t(header[peOffset + 5]) << 8);
                if (machine != 0x14c && machine != 0x8664 && machine != 0) {
                    outTypeName = "PE Executable (non-x86 architecture)";
                    return FileCategory::Generic_Binary;
                }
            }
            outTypeName = "PE Executable (x86/x64)";
            return FileCategory::Executable_PE;
        }
        if (ext == ".exe" || ext == ".dll" || ext == ".sys" || ext == ".ocx") {
            outTypeName = "PE Executable";
            return FileCategory::Executable_PE;
        }
    }

    // 2. Uncompressed Audio / Bitmaps (WAV, BMP, TGA)
    if (bytesRead >= 12 && header[0] == 'R' && header[1] == 'I' && header[2] == 'F' &&
        header[3] == 'F' && header[8] == 'W' && header[9] == 'A' && header[10] == 'V' &&
        header[11] == 'E') {
        outTypeName = "Uncompressed WAV Audio";
        return FileCategory::Uncompressed_Media;
    }
    if (bytesRead >= 2 && header[0] == 'B' && header[1] == 'M') {
        outTypeName = "Bitmap Image (BMP)";
        return FileCategory::Uncompressed_Media;
    }
    if (ext == ".wav" || ext == ".bmp" || ext == ".tga" || ext == ".pcm" || ext == ".raw") {
        outTypeName = "Uncompressed Media / Audio";
        return FileCategory::Uncompressed_Media;
    }

    // 3. High Entropy Precompressed (Video, Compressed Archives, Lossy Images)
    // JPEG: FF D8 FF
    if (bytesRead >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
        outTypeName = "JPEG Image";
        return FileCategory::Precompressed_HighEntropy;
    }
    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (bytesRead >= 8 && std::memcmp(header, "\x89PNG\r\n\x1a\n", 8) == 0) {
        outTypeName = "PNG Image (Precompressed Deflate)";
        return FileCategory::Precompressed_HighEntropy;
    }
    // 7z: 37 7A BC AF 27 1C
    if (bytesRead >= 6 && std::memcmp(header, "7z\xbc\xaf\x27\x1c", 6) == 0) {
        outTypeName = "7-Zip Archive";
        return FileCategory::Precompressed_HighEntropy;
    }
    // RAR: 52 61 72 21 1A 07
    if (bytesRead >= 7 && (std::memcmp(header, "Rar!\x1a\x07\x00", 7) == 0 ||
                           std::memcmp(header, "Rar!\x1a\x07\x01", 7) == 0)) {
        outTypeName = "RAR Archive";
        return FileCategory::Precompressed_HighEntropy;
    }
    // Matroska / MKV: 1A 45 DF A3
    if (bytesRead >= 4 && std::memcmp(header, "\x1a\x45\xdf\xa3", 4) == 0) {
        outTypeName = "Matroska Video (MKV)";
        return FileCategory::Precompressed_HighEntropy;
    }
    // MP4 / MOV: ftyp at offset 4
    if (bytesRead >= 8 && std::memcmp(header + 4, "ftyp", 4) == 0) {
        outTypeName = "MP4 / QuickTime Video";
        return FileCategory::Precompressed_HighEntropy;
    }

    // Check extensions for precompressed files
    if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".webm" || ext == ".mp3" ||
        ext == ".aac" || ext == ".flac" || ext == ".ogg" || ext == ".jpg" || ext == ".jpeg" ||
        ext == ".png" || ext == ".webp" || ext == ".zip" || ext == ".rar" || ext == ".7z" ||
        ext == ".gz" || ext == ".xz") {
        if (entropy > 7.1) {
            outTypeName = "Precompressed Stream (" + ext + ")";
            return FileCategory::Precompressed_HighEntropy;
        }
    }

    // 4. Structured Text / Databases
    // SQLite 3: "SQLite format 3\000"
    if (bytesRead >= 16 && std::memcmp(header, "SQLite format 3\0", 16) == 0) {
        outTypeName = "SQLite 3 Database";
        return FileCategory::Text_Database;
    }
    // Check if mostly printable ASCII/UTF-8
    size_t printableCount = 0;
    for (size_t i = 0; i < bytesRead; ++i) {
        uint8_t b = header[i];
        if ((b >= 32 && b <= 126) || b == '\t' || b == '\r' || b == '\n') {
            printableCount++;
        }
    }
    double textRatio = bytesRead > 0 ? static_cast<double>(printableCount) / bytesRead : 0.0;
    if (textRatio > 0.88 || ext == ".txt" || ext == ".json" || ext == ".xml" || ext == ".csv" ||
        ext == ".sql" || ext == ".html" || ext == ".htm" || ext == ".css" || ext == ".js" ||
        ext == ".ts" || ext == ".cpp" || ext == ".hpp" || ext == ".c" || ext == ".h" ||
        ext == ".md" || ext == ".log") {
        outTypeName = "Structured Text / Source / Data";
        return FileCategory::Text_Database;
    }

    outTypeName = "Generic Binary";
    return FileCategory::Generic_Binary;
}

FileTypeInfo ContentAnalyzer::inspectFile(const std::filesystem::path &filePath, bool hashContents,
                                          const std::atomic<bool> *cancel, bool similarity) {
    FileTypeInfo info;
    info.path = filePath;
#ifdef _WIN32
    auto identity = [&] {
        auto handle = CreateFileW(filePath.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot inspect input file identity");
        BY_HANDLE_FILE_INFORMATION metadata{};
        bool ok = GetFileInformationByHandle(handle, &metadata) != FALSE;
        CloseHandle(handle);
        if (!ok)
            throw std::runtime_error("Cannot inspect input file identity");
        return std::to_string(metadata.dwVolumeSerialNumber) + ":" +
               std::to_string(metadata.nFileIndexHigh) + ":" +
               std::to_string(metadata.nFileIndexLow);
    };
    info.fileIdentity = identity();
    auto attributes = GetFileAttributesW(filePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw std::runtime_error("Unsupported input reparse point: " + pathUtf8(filePath));
    WIN32_FIND_STREAM_DATA stream{};
    HANDLE search = FindFirstStreamW(filePath.c_str(), FindStreamInfoStandard, &stream, 0);
    if (search != INVALID_HANDLE_VALUE) {
        bool alternate = false;
        do {
            if (std::wstring(stream.cStreamName) != L"::$DATA")
                alternate = true;
        } while (FindNextStreamW(search, &stream));
        auto lastError = GetLastError();
        FindClose(search);
        if (lastError != ERROR_HANDLE_EOF)
            throw std::runtime_error("Cannot enumerate input streams");
        if (alternate)
            throw std::runtime_error("NTFS alternate data streams unsupported: " +
                                     pathUtf8(filePath));
    } else if (GetLastError() != ERROR_HANDLE_EOF && GetLastError() != ERROR_INVALID_FUNCTION &&
               GetLastError() != ERROR_NOT_SUPPORTED)
        throw std::runtime_error("Cannot enumerate NTFS streams");
#endif

    std::error_code ec;
    info.sizeBytes = std::filesystem::file_size(filePath, ec);
    if (ec) {
        throw std::runtime_error("Cannot stat input: " + pathUtf8(filePath));
    }

    auto before = std::filesystem::last_write_time(filePath);
    info.modifiedTicks = before.time_since_epoch().count();
    std::ifstream file(filePath, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read input: " + pathUtf8(filePath));

    constexpr size_t SAMPLE_SIZE = 65536; // 64 KB sample for accurate entropy and magic detection
    std::vector<uint8_t> buffer(SAMPLE_SIZE);
    file.read(reinterpret_cast<char *>(buffer.data()), SAMPLE_SIZE);
    size_t bytesRead = static_cast<size_t>(file.gcount());
    buffer.resize(bytesRead);

    info.entropy = calculateEntropy(buffer.data(), bytesRead);
    info.category =
        detectCategory(buffer.data(), bytesRead, filePath, info.entropy, info.detectedType);
    if (similarity) {
        ContentSimilarity::addSample(info.similaritySketch, buffer.data(), bytesRead);
        // Additional bounded reads happen only when the experimental competitor is requested.
        if (info.sizeBytes > SAMPLE_SIZE * 2) {
            std::array<uint8_t, SAMPLE_SIZE> extra{};
            for (auto offset : {(info.sizeBytes - SAMPLE_SIZE) / 2, info.sizeBytes - SAMPLE_SIZE}) {
                if (cancel && cancel->load()) throw std::runtime_error("Cancelled");
                file.clear();
                file.seekg(static_cast<std::streamoff>(offset));
                if (!file.read(reinterpret_cast<char *>(extra.data()), extra.size()))
                    throw std::runtime_error("Input changed while sampling similarity");
                ContentSimilarity::addSample(info.similaritySketch, extra.data(), extra.size());
            }
        }
        file.clear();
        file.seekg(static_cast<std::streamoff>(bytesRead));
    }

    if (hashContents) {
        Hasher hash;
        hash.update(buffer.data(), bytesRead);
        std::array<char, 262144> chunk{};
        uint64_t count = bytesRead;
        while (file.read(chunk.data(), chunk.size()) || file.gcount()) {
            if (cancel && cancel->load())
                throw std::runtime_error("Cancelled");
            hash.update(chunk.data(), static_cast<size_t>(file.gcount()));
            count += static_cast<uint64_t>(file.gcount());
        }
        if (!file.eof() || count != info.sizeBytes ||
            std::filesystem::last_write_time(filePath) != before)
            throw std::runtime_error("Input changed while reading: " + pathUtf8(filePath));
        info.contentChecksum = hash.finish();
    }
#ifdef _WIN32
    if (identity() != info.fileIdentity)
        throw std::runtime_error("Input file replaced while reading: " + pathUtf8(filePath));
#endif
    return info;
}

FileTypeInfo ContentAnalyzer::inspectMemory(const std::filesystem::path &logicalPath,
    std::shared_ptr<const MemoryContent> source, const std::atomic<bool> *cancel, bool similarity) {
    if (!source || !source->owner || !source->data || source->size > source->capacity ||
        source->offset > source->capacity - source->size)
        throw std::runtime_error("Invalid bounded memory content source");
    FileTypeInfo info;
    info.path = logicalPath;
    info.sizeBytes = source->size;
    info.memoryContent = std::move(source);
    const auto &memory = *info.memoryContent;
    auto sample = static_cast<size_t>(std::min<uint64_t>(65536, memory.size));
    info.entropy = calculateEntropy(memory.data, sample);
    info.category = detectCategory(memory.data, sample, logicalPath, info.entropy, info.detectedType);
    if (similarity) {
        ContentSimilarity::addSample(info.similaritySketch, memory.data, sample);
        if (memory.size > 2 * 65536)
            for (auto offset : {(memory.size - 65536) / 2, memory.size - 65536})
                ContentSimilarity::addSample(info.similaritySketch, memory.data + offset, 65536);
    }
    Hasher hash;
    for (uint64_t offset = 0; offset < memory.size;) {
        if (cancel && cancel->load()) throw std::runtime_error("Cancelled");
        auto count = std::min<uint64_t>(262144, memory.size - offset);
        hash.update(memory.data + offset, static_cast<size_t>(count));
        offset += count;
    }
    info.contentChecksum = hash.finish();
    info.fileIdentity = "ram:" + info.contentChecksum;
    return info;
}
AnalysisSummary ContentAnalyzer::analyze(const std::filesystem::path &targetPath, bool hashContents,
                                         const std::atomic<bool> *cancel,
                                         std::function<void(uint64_t, uint64_t)> progress,
                                         bool similarity) {
    AnalysisSummary summary;
    std::error_code ec;

    if (!std::filesystem::exists(targetPath, ec)) {
        return summary;
    }

    double totalEntropySum = 0.0;
    std::set<std::filesystem::path> directories, nonempty;
    auto lastProgress = std::chrono::steady_clock::now();

    auto processSingleFile = [&](const std::filesystem::path &p) {
        if (cancel && cancel->load())
            throw std::runtime_error("Cancelled");
        FileTypeInfo info = inspectFile(p, hashContents, cancel, similarity);
        summary.totalFiles++;
        summary.totalBytes += info.sizeBytes;
        summary.categoryBytes[info.category] += info.sizeBytes;
        summary.categoryCount[info.category]++;
        totalEntropySum += info.entropy * static_cast<double>(info.sizeBytes);

        if (info.category == FileCategory::Executable_PE)
            summary.hasExecutables = true;
        if (info.category == FileCategory::Uncompressed_Media)
            summary.hasUncompressedMedia = true;
        if (info.category == FileCategory::Text_Database)
            summary.hasStructuredText = true;

        // Detect potential deflate streams in ZIP/PDF/APK
        std::string ext = toLower(pathUtf8(p.extension()));
        if (ext == ".zip" || ext == ".apk" || ext == ".pdf" || ext == ".jar") {
            summary.hasDeflateStreams = true;
        }

        summary.files.push_back(info);
        if (progress &&
            std::chrono::steady_clock::now() - lastProgress >= std::chrono::milliseconds(100)) {
            progress(summary.totalFiles, summary.totalBytes);
            lastProgress = std::chrono::steady_clock::now();
        }
    };

    if (std::filesystem::is_directory(targetPath, ec)) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(
                 targetPath, std::filesystem::directory_options::none, ec)) {
            if (cancel && cancel->load())
                throw std::runtime_error("Cancelled");
#ifdef _WIN32
            auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
                throw std::runtime_error("Cannot inspect NTFS attributes");
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                throw std::runtime_error("Reparse points are not supported: " +
                                         pathUtf8(entry.path()));
#endif
            nonempty.insert(entry.path().parent_path());
            if (entry.is_directory())
                directories.insert(entry.path());
            if (entry.is_symlink())
                throw std::runtime_error("Symbolic links are not supported: " +
                                         pathUtf8(entry.path()));
            if (entry.is_regular_file(ec)) {
                processSingleFile(entry.path());
            }
        }
    } else if (std::filesystem::is_regular_file(targetPath, ec)) {
        processSingleFile(targetPath);
    }

    if (ec)
        throw std::filesystem::filesystem_error("Input scan failed", targetPath, ec);
    std::sort(summary.files.begin(), summary.files.end(),
              [](const auto &a, const auto &b) { return a.path < b.path; });
    if (summary.totalFiles > 0) {
        summary.averageEntropy = summary.totalBytes ? totalEntropySum / summary.totalBytes : 0.0;
    }

    for (const auto &dir : directories)
        if (!nonempty.count(dir))
            summary.emptyDirectories.push_back(dir);
    if (progress)
        progress(summary.totalFiles, summary.totalBytes);

    // Determine dominant category by total bytes
    uint64_t maxBytes = 0;
    for (const auto &[cat, bytes] : summary.categoryBytes) {
        if (bytes > maxBytes) {
            maxBytes = bytes;
            summary.dominantCategory = cat;
        }
    }

    return summary;
}

} // namespace UltraArch
