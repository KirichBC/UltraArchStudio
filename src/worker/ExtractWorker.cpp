// Single-pass hybrid extraction: bounded RAM content and private disk spill.
#include "ExtractWorkerProtocol.hpp"
#include <cstring>
#include "core/FileUtils.hpp"
#include "core/BackendIdentity.hpp"
#include "core/WorkspaceManager.hpp"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <objidl.h>
#include <oleauto.h>
#include <propvarutil.h>
#include <set>
#include <initguid.h>
#include "7zip/CPP/7zip/Archive/IArchive.h"
#include "DecoderThreads.hpp"
#include "7zip/CPP/7zip/IPassword.h"
using namespace UltraArch;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
constexpr GUID format7z = {0x23170f69, 0x40c1, 0x278a, {0x10, 0, 0, 1, 0x10, 7, 0, 0}};
std::string normalized(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    WorkspaceManager::validateArchivePath(path);
    return path;
}
struct RefCount {
    std::atomic<ULONG> refs{1};
};
#define REF_METHODS(type)                                                                          \
    ULONG STDMETHODCALLTYPE AddRef() noexcept override {                                           \
        return ++refs;                                                                             \
    }                                                                                              \
    ULONG STDMETHODCALLTYPE Release() noexcept override {                                          \
        auto n = --refs;                                                                           \
        if (!n)                                                                                    \
            delete this;                                                                           \
        return n;                                                                                  \
    }
template <class T> struct Owner {
    T *p = nullptr;
    ~Owner() {
        if (p)
            p->Release();
    }
    T *operator->() {
        return p;
    }
};
class OpenCallback final : public IArchiveOpenCallback,
                           public ICryptoGetTextPassword,
                           public RefCount {
    std::wstring password_;

  public:
    explicit OpenCallback(const std::string &password) : password_(utf8Path(password).wstring()) {}
    REF_METHODS(OpenCallback)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) noexcept override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IArchiveOpenCallback)
            *out = static_cast<IArchiveOpenCallback *>(this);
        else if (iid == IID_ICryptoGetTextPassword)
            *out = static_cast<ICryptoGetTextPassword *>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(const UInt64 *, const UInt64 *) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64 *, const UInt64 *) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR *password) noexcept override {
        *password = SysAllocStringLen(password_.data(), static_cast<UINT>(password_.size()));
        return *password ? S_OK : E_OUTOFMEMORY;
    }
};
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct View {
    void *data = nullptr;
    ~View() { if (data) UnmapViewOfFile(data); }
};
struct Fallback : std::runtime_error { using std::runtime_error::runtime_error; };
std::string text(const Json &value) {
    auto result = value.get<std::string>();
    if (result.find('\0') != std::string::npos) throw std::runtime_error("Embedded NUL");
    return result;
}
void safeAncestors(const fs::path &path) {
    if (!path.is_absolute()) throw std::runtime_error("Absolute path required");
    for (auto ancestor = path; !ancestor.empty(); ancestor = ancestor.parent_path()) {
        if (isLinkOrReparse(ancestor)) throw std::runtime_error("Reparse path unsupported");
        if (ancestor == ancestor.parent_path()) break;
    }
}
class ArchiveStream final : public IInStream, public RefCount {
    Handle handle_;
    uint64_t size_ = 0;
  public:
    explicit ArchiveStream(const fs::path &path) {
        safeAncestors(path);
        handle_.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle_.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot lock source archive");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle_.value, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            throw std::runtime_error("Invalid source archive");
        size_ = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        const unsigned char signature[6] = {0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c};
        unsigned char data[6]{}; DWORD read = 0;
        if (!ReadFile(handle_.value, data, 6, &read, nullptr)) throw std::runtime_error("Cannot read archive signature");
        if (read != 6 || std::memcmp(data, signature, 6)) {
            if (read >= 2 && data[0] == 'M' && data[1] == 'Z')
                throw Fallback("Hybrid SFX input uses disk extraction");
            // An explicitly named .7z with damaged/truncated magic is a failure,
            // not an unsupported-format escape after corruption.
            if (archivePathKey(pathUtf8(path.extension())) == ".7Z")
                throw std::runtime_error("Corrupt/truncated 7z signature");
            throw Fallback("Hybrid input supports ordinary 7z archives only");
        }
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(handle_.value, zero, nullptr, FILE_BEGIN)) throw std::runtime_error("Cannot seek archive");
    }
    REF_METHODS(ArchiveStream)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) noexcept override {
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IInStream && iid != IID_ISequentialInStream) return E_NOINTERFACE;
        *out = static_cast<IInStream *>(this); AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Read(void *data, UInt32 size, UInt32 *processed) noexcept override {
        DWORD read = 0;
        auto success = ReadFile(handle_.value, data, size, &read, nullptr);
        if (processed) *processed = read;
        return success ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64 *result) noexcept override {
        if (origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
        LARGE_INTEGER off{}, position{}; off.QuadPart = offset;
        if (!SetFilePointerEx(handle_.value, off, &position, origin)) return HRESULT_FROM_WIN32(GetLastError());
        if (result) *result = static_cast<UInt64>(position.QuadPart);
        return S_OK;
    }
};
struct Property {
    PROPVARIANT value{};
    Property(IInArchive *archive, UInt32 index, PROPID id) {
        PropVariantInit(&value);
        auto status = archive->GetProperty(index, id, &value);
        if (status != S_OK) {
            PropVariantClear(&value);
            if (status == E_OUTOFMEMORY) throw std::bad_alloc();
            throw std::runtime_error("Cannot inspect archive property");
        }
    }
    ~Property() { PropVariantClear(&value); }
};
uint64_t archiveTime(IInArchive *archive, UInt32 index, PROPID id) {
    Property prop(archive, index, id);
    if (prop.value.vt == VT_EMPTY) return 0;
    if (prop.value.vt != VT_FILETIME) throw std::runtime_error("Invalid archive timestamp");
    return (uint64_t(prop.value.filetime.dwHighDateTime) << 32) | prop.value.filetime.dwLowDateTime;
}
FILETIME toTime(uint64_t ticks) {
    return FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
}
struct Entry {
    std::string relative, sha;
    uint64_t size = 0, offset = 0, mtime = 0, ctime = 0, atime = 0;
    uint32_t attributes = 0;
    bool directory = false, ram = false, complete = false;
};
std::string extension(const std::string &relative) {
    auto ext = pathUtf8(utf8Path(relative).extension());
    for (auto &ch : ext) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return archivePathKey(ext);
}
std::vector<Entry> preflight(IInArchive *archive, uint64_t totalLimit, uint64_t fileLimit,
                             uint64_t entryLimit, std::map<std::string, std::string> &directories) {
    UInt32 count = 0;
    auto status = archive->GetNumberOfItems(&count);
    if (status == E_OUTOFMEMORY) throw std::bad_alloc();
    if (status != S_OK || count > entryLimit) throw std::runtime_error("Archive entry count quota exceeded");
    std::vector<Entry> entries;
    entries.reserve(count);
    std::map<std::string, bool> kinds;
    uint64_t total = 0;
    for (UInt32 index = 0; index < count; ++index) {
        Entry entry;
        Property path(archive, index, kpidPath);
        if (path.value.vt != VT_BSTR || SysStringLen(path.value.bstrVal) > 32760)
            throw std::runtime_error("Invalid archive path");
        auto wide = std::wstring(path.value.bstrVal, SysStringLen(path.value.bstrVal));
        if (wide.find(L'\0') != std::wstring::npos) throw std::runtime_error("Embedded NUL in archive name");
        entry.relative = normalized(pathUtf8(fs::path(wide)));
        if (entry.relative.size() > 131040) throw std::runtime_error("Archive path too long");
        Property directory(archive, index, kpidIsDir);
        if (directory.value.vt != VT_BOOL && directory.value.vt != VT_EMPTY)
            throw std::runtime_error("Invalid directory property");
        entry.directory = directory.value.vt == VT_BOOL && directory.value.boolVal != VARIANT_FALSE;
        if (!kinds.emplace(archivePathKey(entry.relative), entry.directory).second)
            throw std::runtime_error("Duplicate/case-colliding archive path");
        for (auto property : {kpidSymLink, kpidHardLink, kpidIsAltStream, kpidIsAnti}) {
            Property prop(archive, index, property);
            if ((prop.value.vt == VT_BSTR && SysStringLen(prop.value.bstrVal)) ||
                (prop.value.vt == VT_BOOL && prop.value.boolVal != VARIANT_FALSE) ||
                (prop.value.vt != VT_EMPTY && prop.value.vt != VT_BSTR && prop.value.vt != VT_BOOL))
                throw std::runtime_error("Archive links/alternate streams unsupported");
        }
        Property posix(archive, index, kpidPosixAttrib);
        if (posix.value.vt == VT_UI4) {
            auto type = posix.value.ulVal & 0170000;
            if (type && type != (entry.directory ? 0040000 : 0100000))
                throw std::runtime_error("Nonregular POSIX archive entry unsupported");
        } else if (posix.value.vt != VT_EMPTY) throw std::runtime_error("Invalid POSIX attribute");
        Property attributes(archive, index, kpidAttrib);
        if (attributes.value.vt == VT_UI4) entry.attributes = attributes.value.ulVal;
        else if (attributes.value.vt != VT_EMPTY) throw std::runtime_error("Invalid file attributes");
        if (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Archive reparse attributes unsupported");
        if (entry.attributes & 0x8000) {
            auto type = (entry.attributes >> 16) & 0170000;
            if (type && type != (entry.directory ? 0040000 : 0100000))
                throw std::runtime_error("Archive special filesystem entry unsupported");
        }
        entry.mtime = archiveTime(archive, index, kpidMTime);
        entry.ctime = archiveTime(archive, index, kpidCTime);
        entry.atime = archiveTime(archive, index, kpidATime);
        Property size(archive, index, kpidSize);
        if (!entry.directory) {
            if (size.value.vt != VT_UI8) throw std::runtime_error("Missing archive file size");
            entry.size = size.value.uhVal.QuadPart;
            if (entry.size > fileLimit || entry.size > totalLimit - total)
                throw std::runtime_error("Archive expansion quota exceeded");
            total += entry.size;
        } else if (size.value.vt != VT_EMPTY && (size.value.vt != VT_UI8 || size.value.uhVal.QuadPart))
            throw std::runtime_error("Nonempty directory stream unsupported");
        entries.push_back(std::move(entry));
    }
    for (const auto &entry : entries) {
        auto path = entry.directory ? utf8Path(entry.relative) : utf8Path(entry.relative).parent_path();
        while (!path.empty()) {
            auto name = pathUtf8(path), key = archivePathKey(name);
            if (auto found = kinds.find(key); found != kinds.end() && !found->second)
                throw std::runtime_error("File conflicts with parent directory");
            auto [found, inserted] = directories.emplace(key, name);
            if (!inserted && found->second != name)
                throw std::runtime_error("Case-colliding directory spelling unsupported");
            if (directories.size() > entryLimit) throw std::runtime_error("Directory quota exceeded");
            path = path.parent_path();
        }
    }
    uint64_t files = 0;
    for (const auto &entry : entries) files += !entry.directory;
    if (files > entryLimit - std::min<uint64_t>(entryLimit, directories.size()))
        throw std::runtime_error("Combined file/directory quota exceeded");
    // Parent admits at most 64 MiB of manifest JSON. A conservative bound includes
    // all numeric fields, SHA, storage and maximal JSON escaping before any writes.
    uint64_t manifestBound = 1024;
    auto reserveManifest = [&](const std::string &name, uint64_t fixed) {
        if (name.size() > (ExtractWorkerProtocol::maxManifestBytes - fixed) / 6)
            throw std::runtime_error("Hybrid manifest byte quota exceeded");
        auto bytes = fixed + 6 * static_cast<uint64_t>(name.size());
        if (bytes > ExtractWorkerProtocol::maxManifestBytes - manifestBound)
            throw std::runtime_error("Hybrid manifest byte quota exceeded");
        manifestBound += bytes;
    };
    for (const auto &entry : entries) if (!entry.directory) reserveManifest(entry.relative, 512);
    for (const auto &[key, relative] : directories) reserveManifest(relative, 8);
    return entries;
}
void setMetadata(HANDLE handle, const Entry &entry) {
    auto ctime = toTime(entry.ctime), atime = toTime(entry.atime), mtime = toTime(entry.mtime);
    if (!SetFileTime(handle, entry.ctime ? &ctime : nullptr, entry.atime ? &atime : nullptr,
                     entry.mtime ? &mtime : nullptr))
        throw std::runtime_error("Cannot preserve extracted timestamps");
}
class Sink final : public ISequentialOutStream, public RefCount {
    Entry &entry_;
    unsigned char *ram_;
    Handle file_;
    Hasher hash_;
    uint64_t written_ = 0;
    fs::path path_;
  public:
    Sink(Entry &entry, unsigned char *ram, const fs::path &root) : entry_(entry), ram_(ram) {
        if (!entry_.ram) {
            path_ = archiveEntryPath(root, entry_.relative);
            safeAncestors(path_.parent_path());
            if (!isWithin(path_, root)) throw std::runtime_error("Extraction path escaped root");
            file_.value = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file_.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create private extracted file");
        }
    }
    REF_METHODS(Sink)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) noexcept override {
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ISequentialOutStream) return E_NOINTERFACE;
        *out = static_cast<ISequentialOutStream *>(this); AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void *data, UInt32 size, UInt32 *processed) noexcept override {
        if (processed) *processed = 0;
        try {
            if (size > entry_.size - written_) return E_FAIL;
            if (entry_.ram) {
                if (size) std::memcpy(ram_ + entry_.offset + written_, data, size);
            } else {
                const auto *bytes = static_cast<const unsigned char *>(data);
                DWORD remaining = size;
                while (remaining) {
                    DWORD wrote = 0;
                    if (!WriteFile(file_.value, bytes, remaining, &wrote, nullptr) || !wrote)
                        return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_WRITE_FAULT);
                    bytes += wrote; remaining -= wrote;
                }
            }
            hash_.update(data, size); written_ += size;
            if (processed) *processed = size;
            return S_OK;
        } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }
    void finish() {
        if (written_ != entry_.size) throw std::runtime_error("Decoded size mismatch");
        entry_.sha = hash_.finish();
        if (!entry_.ram) {
            setMetadata(file_.value, entry_);
            if (!FlushFileBuffers(file_.value)) throw std::runtime_error("Cannot flush extracted file");
            CloseHandle(file_.value); file_.value = nullptr;
            auto attributes = entry_.attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE);
            if (attributes && !SetFileAttributesW(path_.c_str(), attributes))
                throw std::runtime_error("Cannot preserve extracted attributes");
        }
        entry_.complete = true;
    }
};
class ExtractCallback final : public IArchiveExtractCallback, public ICryptoGetTextPassword,
                              public RefCount {
    std::vector<Entry> &entries_;
    unsigned char *ram_;
    fs::path root_;
    std::wstring password_;
    Sink *sink_ = nullptr;
    Entry *current_ = nullptr;
    std::set<UInt32> seen_;
  public:
    std::string error;
    bool oom = false;
    ExtractCallback(std::vector<Entry> &entries, unsigned char *ram, fs::path root,
                     const std::string &password)
        : entries_(entries), ram_(ram), root_(std::move(root)), password_(utf8Path(password).wstring()) {}
    ~ExtractCallback() { if (sink_) sink_->Release(); }
    REF_METHODS(ExtractCallback)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) noexcept override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IProgress || iid == IID_IArchiveExtractCallback)
            *out = static_cast<IArchiveExtractCallback *>(this);
        else if (iid == IID_ICryptoGetTextPassword)
            *out = static_cast<ICryptoGetTextPassword *>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64 *) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32 mode) noexcept override {
        return mode == NArchive::NExtract::NAskMode::kExtract ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR *password) noexcept override {
        *password = SysAllocStringLen(password_.data(), static_cast<UINT>(password_.size()));
        return *password ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISequentialOutStream **stream, Int32 mode) noexcept override {
        *stream = nullptr;
        try {
            if (sink_) { sink_->Release(); sink_ = nullptr; }
            if (index >= entries_.size() || !seen_.insert(index).second ||
                mode != NArchive::NExtract::NAskMode::kExtract)
                throw std::runtime_error("Unexpected/repeated decoder entry");
            current_ = &entries_[index];
            if (current_->directory) return S_OK;
            sink_ = new Sink(*current_, ram_, root_); sink_->AddRef(); *stream = sink_;
            return S_OK;
        } catch (const std::bad_alloc &) { oom = true; return E_OUTOFMEMORY; }
        catch (const std::exception &e) { error = e.what(); return E_FAIL; }
        catch (...) { error = "Hybrid extraction stream failed"; return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 result) noexcept override {
        try {
            if (result != NArchive::NExtract::NOperationResult::kOK || !current_)
                throw std::runtime_error("Archive CRC/decompression/password failure");
            if (current_->directory) current_->complete = true;
            else {
                if (!sink_) throw std::runtime_error("Missing extraction sink");
                sink_->finish(); sink_->Release(); sink_ = nullptr;
            }
            current_ = nullptr; return S_OK;
        } catch (const std::bad_alloc &) { oom = true; return E_OUTOFMEMORY; }
        catch (const std::exception &e) { error = e.what(); return E_FAIL; }
        catch (...) { error = "Hybrid extraction result failed"; return E_FAIL; }
    }
    bool complete() const {
        return std::all_of(entries_.begin(), entries_.end(), [](const auto &entry) { return entry.complete; });
    }
};
void readExact(HANDLE input, void *data, DWORD size) {
    while (size) {
        DWORD read = 0;
        if (!ReadFile(input, data, size, &read, nullptr) || !read) throw std::runtime_error("Incomplete hybrid request");
        size -= read; data = static_cast<char *>(data) + read;
    }
}
void writeReply(const Json &reply) {
    auto body = reply.dump();
    if (body.size() > ExtractWorkerProtocol::maxReplyBytes) return;
    std::string frame = ExtractWorkerProtocol::magic;
    auto length = static_cast<uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) frame.push_back(static_cast<char>(length >> (8 * i)));
    frame += body;
    size_t offset = 0;
    while (offset < frame.size()) {
        DWORD wrote = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), frame.data() + offset,
                       static_cast<DWORD>(frame.size() - offset), &wrote, nullptr) || !wrote) return;
        offset += wrote;
    }
}
Json run(const Json &request) {
    if (request.at("schema").get<uint32_t>() != ExtractWorkerProtocol::schema)
        throw std::runtime_error("Unknown hybrid schema");
    auto capacity = request.at("capacityBytes").get<uint64_t>();
    auto groupLimit = request.at("groupLimitBytes").get<uint64_t>();
    auto totalLimit = request.at("totalLimitBytes").get<uint64_t>();
    auto fileLimit = request.at("fileLimitBytes").get<uint64_t>();
    auto entryLimit = request.at("fileCountLimit").get<uint64_t>();
    if (capacity > ExtractWorkerProtocol::maxCapacityBytes || groupLimit > ExtractWorkerProtocol::maxGroupBytes ||
        !entryLimit || entryLimit > ExtractWorkerProtocol::maxEntries)
        throw std::runtime_error("Invalid hybrid quota");
    auto root = utf8Path(text(request.at("root")));
    auto manifestPath = utf8Path(text(request.at("manifestPath")));
    safeAncestors(root); safeAncestors(manifestPath.parent_path());
    if (!fs::is_directory(root) || !fs::is_empty(root) || !manifestPath.is_absolute() ||
        !isWithin(manifestPath, root.parent_path()) || fs::exists(manifestPath))
        throw std::runtime_error("Hybrid extraction requires empty private root and fresh manifest");
    auto source = utf8Path(text(request.at("archive")));
    if (source.extension() == L".001") throw Fallback("Hybrid split input uses disk extraction");
    Owner<ArchiveStream> stream{new ArchiveStream(source)};
    auto dll = utf8Path(text(request.at("dll")));
    safeAncestors(dll);
    Handle dllLock{CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (dllLock.value == INVALID_HANDLE_VALUE || fileChecksum(dll) != pinned7ZipDllSha256)
        throw std::runtime_error("Hybrid extraction requires pinned DLL");
    struct Module { HMODULE value; ~Module() { if (value) FreeLibrary(value); } } module{
        LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!module.value) throw std::runtime_error("Cannot load hybrid decoder");
    using Create = HRESULT(WINAPI *)(const GUID *, const GUID *, void **);
    auto create = reinterpret_cast<Create>(GetProcAddress(module.value, "CreateObject"));
    Owner<IInArchive> archive;
    if (!create) throw std::runtime_error("Missing decoder factory");
    auto status = create(&format7z, &IID_IInArchive, reinterpret_cast<void **>(&archive.p));
    if (status == E_OUTOFMEMORY) throw std::bad_alloc();
    if (status != S_OK) throw std::runtime_error("Cannot create hybrid decoder");
    limitDecoderThreads(archive.p);
    auto password = request.contains("password") ? text(request.at("password")) : std::string{};
    Owner<OpenCallback> open{new OpenCallback(password)};
    UInt64 scanLimit = 0;
    status = archive->Open(stream.p, &scanLimit, open.p);
    if (status == E_OUTOFMEMORY) throw std::bad_alloc();
    if (status != S_OK) throw std::runtime_error("Corrupt 7z header or incorrect password");
    std::map<std::string, std::string> directories;
    auto entries = preflight(archive.p, totalLimit, fileLimit, entryLimit, directories);
    std::map<std::string, uint64_t> groups;
    for (const auto &entry : entries) if (!entry.directory) groups[extension(entry.relative)] += entry.size;
    uint64_t ramBytes = 0, diskBytes = 0;
    std::set<std::string> ramGroups;
    for (const auto &[key, size] : groups) {
        if (groupLimit && size <= groupLimit && size <= capacity - ramBytes) {
            ramGroups.insert(key); ramBytes += size;
        }
    }
    uint64_t offset = 0;
    for (auto &entry : entries) if (!entry.directory) {
        entry.ram = ramGroups.count(extension(entry.relative)) != 0;
        if (entry.ram) { entry.offset = offset; offset += entry.size; }
        else diskBytes += entry.size;
    }
    Handle mapping;
    View view;
    if (ramBytes) {
        auto name = utf8Path(text(request.at("mappingName"))).wstring();
        if (name.empty() || name.size() > 240) throw std::runtime_error("Invalid hybrid mapping name");
        mapping.value = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
        if (!mapping.value) throw std::runtime_error("Cannot open hybrid RAM mapping");
        view.data = MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(capacity));
        if (!view.data) throw std::runtime_error("Cannot map hybrid RAM input");
    }
    // All archive names and quotas were checked before these first filesystem writes.
    for (const auto &[key, relative] : directories) {
        auto path = archiveEntryPath(root, relative);
        if (!isWithin(path, root)) throw std::runtime_error("Directory escaped private root");
        safeAncestors(path.parent_path());
        fs::create_directories(path);
        safeAncestors(path);
    }
    Owner<ExtractCallback> callback{new ExtractCallback(entries, static_cast<unsigned char *>(view.data), root, password)};
    // Exactly one extraction of all entries: no repeated decoding of solid blocks.
    status = archive->Extract(nullptr, static_cast<UInt32>(-1), 0, callback.p);
    archive->Close();
    if (status == E_OUTOFMEMORY || callback->oom) throw std::bad_alloc();
    if (status != S_OK || !callback->error.empty() || !callback->complete())
        throw std::runtime_error(callback->error.empty() ? "Incomplete hybrid extraction" : callback->error);
    for (const auto &entry : entries) if (entry.directory) {
        auto path = archiveEntryPath(root, entry.relative);
        safeAncestors(path);
        Handle handle{CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (handle.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot set directory metadata");
        setMetadata(handle.value, entry);
    }
    Json manifest{{"schema", 1}, {"files", Json::array()}, {"directories", Json::array()},
                  {"ramBytes", ramBytes}, {"diskBytes", diskBytes}};
    for (const auto &entry : entries) if (!entry.directory) {
        manifest["files"].push_back({{"relative", entry.relative}, {"size", entry.size}, {"sha256", entry.sha},
            {"storage", entry.ram ? "ram" : "disk"}, {"offset", entry.offset}, {"attributes", entry.attributes},
            {"mtime", entry.mtime}, {"ctime", entry.ctime}, {"atime", entry.atime}});
    }
    for (const auto &[key, relative] : directories) manifest["directories"].push_back(relative);
    auto serialized = manifest.dump();
    if (serialized.size() > ExtractWorkerProtocol::maxManifestBytes)
        throw std::runtime_error("Hybrid manifest byte quota exceeded");
    if (fs::exists(manifestPath) || !writeAtomically(manifestPath, serialized))
        throw std::runtime_error("Cannot publish hybrid manifest");
    return {{"schema", 1}, {"success", true}, {"fallback", false}, {"error", ""},
            {"manifestSha256", fileChecksum(manifestPath)}, {"ramBytes", ramBytes}, {"diskBytes", diskBytes}};
}
} // namespace
int main() {
    try {
        char header[8]; readExact(GetStdHandle(STD_INPUT_HANDLE), header, 8);
        if (std::string(header, 4) != ExtractWorkerProtocol::magic) throw std::runtime_error("Invalid hybrid protocol");
        uint32_t length = 0;
        for (int i = 0; i < 4; ++i) length |= uint32_t(static_cast<unsigned char>(header[4 + i])) << (8 * i);
        if (!length || length > ExtractWorkerProtocol::maxRequestBytes) throw std::runtime_error("Hybrid request too large");
        std::string body(length, '\0'); readExact(GetStdHandle(STD_INPUT_HANDLE), body.data(), length);
        writeReply(run(Json::parse(body))); return 0;
    } catch (const std::bad_alloc &) {
        try { writeReply({{"schema", 1}, {"success", false}, {"fallback", false}, {"error", "Out of memory"},
                          {"manifestSha256", ""}, {"ramBytes", 0}, {"diskBytes", 0}}); } catch (...) {}
        return 8;
    } catch (const std::exception &e) {
        writeReply({{"schema", 1}, {"success", false}, {"fallback", dynamic_cast<const Fallback *>(&e) != nullptr},
                    {"error", std::string(e.what()).substr(0, 4096)}, {"manifestSha256", ""}, {"ramBytes", 0}, {"diskBytes", 0}});
        return 1;
    } catch (...) {
        writeReply({{"schema", 1}, {"success", false}, {"fallback", false}, {"error", "Hybrid extraction failed"},
                    {"manifestSha256", ""}, {"ramBytes", 0}, {"diskBytes", 0}});
        return 1;
    }
}
