#include "InputManifest.hpp"
#include <nlohmann/json.hpp>
#include "WorkspaceManager.hpp"
namespace UltraArch {
InputManifest InputManifest::scan(const std::filesystem::path &input,
                                  const std::atomic<bool> *cancel,
                                  std::function<void(uint64_t, uint64_t)> progress, bool similarity) {
    InputManifest manifest;
    manifest.root = std::filesystem::is_directory(input) ? input : input.parent_path();
    ContentAnalyzer analyzer;
    manifest.summary = analyzer.analyze(input, true, cancel, std::move(progress), similarity);
    return fromEntries(manifest.root, std::move(manifest.summary.files), std::move(manifest.summary.emptyDirectories));
}
InputManifest InputManifest::fromEntries(const std::filesystem::path &root,
    std::vector<FileTypeInfo> files, std::vector<std::filesystem::path> emptyDirectories) {
    InputManifest manifest;
    manifest.root = root;
    manifest.summary.files = std::move(files);
    manifest.summary.emptyDirectories = std::move(emptyDirectories);
    std::sort(manifest.summary.files.begin(), manifest.summary.files.end(),
        [](const auto &a, const auto &b) { return a.path < b.path; });
    double entropy = 0;
    for (const auto &file : manifest.summary.files) {
        if (file.sizeBytes > UINT64_MAX - manifest.summary.totalBytes)
            throw std::runtime_error("Manifest size overflow");
        ++manifest.summary.totalFiles;
        manifest.summary.totalBytes += file.sizeBytes;
        manifest.summary.categoryBytes[file.category] += file.sizeBytes;
        ++manifest.summary.categoryCount[file.category];
        entropy += file.entropy * static_cast<double>(file.sizeBytes);
        manifest.summary.hasExecutables |= file.category == FileCategory::Executable_PE;
        manifest.summary.hasUncompressedMedia |= file.category == FileCategory::Uncompressed_Media;
        manifest.summary.hasStructuredText |= file.category == FileCategory::Text_Database;
        auto ext = pathUtf8(file.path.extension());
        for (auto &ch : ext) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        manifest.summary.hasDeflateStreams |= ext == ".zip" || ext == ".apk" || ext == ".pdf" || ext == ".jar";
    }
    uint64_t largest = 0;
    for (const auto &[category, bytes] : manifest.summary.categoryBytes)
        if (bytes > largest) { largest = bytes; manifest.summary.dominantCategory = category; }
    manifest.summary.averageEntropy = manifest.summary.totalBytes ? entropy / manifest.summary.totalBytes : 0;
    nlohmann::json entries = nlohmann::json::array();
    std::set<std::string> names;
    for (const auto &file : manifest.summary.files) {
        auto relative = pathUtf8(file.path.lexically_relative(manifest.root));
        WorkspaceManager::validateArchivePath(relative);
        if (!names.insert(archivePathKey(relative)).second)
            throw std::runtime_error("Input contains names colliding on Windows");
        entries.push_back({pathUtf8(file.path.lexically_relative(manifest.root)), "file",
                           file.sizeBytes, file.contentChecksum});
        manifest.bytesRead += file.sizeBytes;
    }
    for (const auto &dir : manifest.summary.emptyDirectories)
        entries.push_back({pathUtf8(dir.lexically_relative(manifest.root)), "directory"});
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return a.at(0).template get<std::string>() < b.at(0).template get<std::string>();
    });
    manifest.fingerprint = Hasher::text(std::to_string(schema) + entries.dump());
    return manifest;
}
} // namespace UltraArch
