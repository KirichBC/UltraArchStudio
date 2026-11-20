// Isolated bounded RAM encoder and SHA-verifying decoder. No candidate bytes touch disk.
#include "EncodeWorkerProtocol.hpp"
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
HRESULT seekPosition(uint64_t &position, uint64_t size, Int64 offset, UInt32 origin,
                     UInt64 *result, uint64_t ceiling = UINT64_MAX) noexcept {
    if (origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
    auto base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? position : size;
    auto magnitude = offset < 0 ? uint64_t(-(offset + 1)) + 1 : uint64_t(offset);
    if ((offset < 0 && magnitude > base) || (offset >= 0 && magnitude > UINT64_MAX - base))
        return STG_E_INVALIDFUNCTION;
    auto next = offset < 0 ? base - magnitude : base + magnitude;
    if (next > ceiling) return STG_E_MEDIUMFULL;
    position = next;
    if (result) *result = position;
    return S_OK;
}
class MemoryInput final : public IInStream, public IStreamGetSize, public RefCount {
    const unsigned char *data_;
    uint64_t size_, position_ = 0;
  public:
    MemoryInput(const unsigned char *data, uint64_t size) : data_(data), size_(size) {}
    REF_METHODS(MemoryInput)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) noexcept override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IInStream || iid == IID_ISequentialInStream)
            *out = static_cast<IInStream *>(this);
        else if (iid == IID_IStreamGetSize)
            *out = static_cast<IStreamGetSize *>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Read(void *data, UInt32 size, UInt32 *processed) noexcept override {
        auto n = static_cast<UInt32>(std::min<uint64_t>(size, position_ >= size_ ? 0 : size_ - position_));
        if (n) std::memcpy(data, data_ + position_, n);
        position_ += n;
        if (processed) *processed = n;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 off, UInt32 origin, UInt64 *result) noexcept override {
        return seekPosition(position_, size_, off, origin, result);
    }
    HRESULT STDMETHODCALLTYPE GetSize(UInt64 *size) noexcept override {
        // BCJ2 uses substream size to bound eligible relative branch targets.
        // Omitting it changes compressed bytes compared with CLI file streams.
        *size = size_; return S_OK;
    }
};
class MappingOutput final : public IOutStream, public RefCount {
    unsigned char *data_;
    uint64_t capacity_, position_ = 0;
  public:
    uint64_t bytes = 0;
    bool overflow = false;
    MappingOutput(unsigned char *data, uint64_t capacity) : data_(data), capacity_(capacity) {}
    REF_METHODS(MappingOutput)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) noexcept override {
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IOutStream && iid != IID_ISequentialOutStream)
            return E_NOINTERFACE;
        *out = static_cast<IOutStream *>(this); AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void *data, UInt32 size, UInt32 *processed) noexcept override {
        if (processed) *processed = 0;
        if (size > capacity_ - position_) { overflow = true; return STG_E_MEDIUMFULL; }
        if (position_ > bytes) std::memset(data_ + bytes, 0, static_cast<size_t>(position_ - bytes));
        if (size) std::memcpy(data_ + position_, data, size);
        position_ += size; bytes = std::max(bytes, position_);
        if (processed) *processed = size;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 off, UInt32 origin, UInt64 *result) noexcept override {
        auto status = seekPosition(position_, bytes, off, origin, result, capacity_);
        if (status == STG_E_MEDIUMFULL) overflow = true;
        return status;
    }
    HRESULT STDMETHODCALLTYPE SetSize(UInt64 size) noexcept override {
        if (size > capacity_) { overflow = true; return STG_E_MEDIUMFULL; }
        if (size > bytes) std::memset(data_ + bytes, 0, static_cast<size_t>(size - bytes));
        bytes = size; return S_OK;
    }
};
class HashSink final : public ISequentialOutStream, public RefCount {
  public:
    Hasher hash;
    uint64_t written = 0, expected = 0;
    explicit HashSink(uint64_t size) : expected(size) {}
    REF_METHODS(HashSink)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) noexcept override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_ISequentialOutStream) {
            *out = static_cast<ISequentialOutStream *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void *data, UInt32 size,
                                    UInt32 *processed) noexcept override {
        if (processed)
            *processed = 0;
        try {
            if (size > expected - written)
                return E_FAIL;
            hash.update(data, size);
            written += size;
            if (processed)
                *processed = size;
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
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
struct Expected {
    uint64_t size;
    std::string hash;
};
class ExtractCallback final : public IArchiveExtractCallback,
                              public ICryptoGetTextPassword,
                              public RefCount {
    IInArchive *archive_;
    std::wstring password_;
    std::map<std::string, Expected> files_;
    std::set<std::string> directories_, allowedDirs_, seen_, seenDirs_;
    HashSink *sink_ = nullptr;
    std::string current_, expectedHash_;
    bool directory_ = false;

  public:
    std::string error;
    uint64_t verified = 0, bytes = 0;
    ExtractCallback(IInArchive *archive, const Json &manifest, const std::string &password)
        : archive_(archive), password_(utf8Path(password).wstring()) {
        for (const auto &entry : manifest.at("files")) {
            auto path = normalized(entry.at("path").get<std::string>());
            auto hash = entry.at("sha256").get<std::string>();
            if (hash.size() != 64 ||
                !files_.emplace(path, Expected{entry.at("size").get<uint64_t>(), hash}).second)
                throw std::runtime_error("Invalid expected manifest");
        }
        for (const auto &entry : manifest.at("directories"))
            directories_.insert(normalized(entry.get<std::string>()));
        allowedDirs_ = directories_;
        for (const auto &[path, expected] : files_) {
            auto parent = utf8Path(path).parent_path();
            while (!parent.empty()) {
                allowedDirs_.insert(pathUtf8(parent));
                parent = parent.parent_path();
            }
        }
        for (const auto &directory : directories_) {
            auto parent = utf8Path(directory).parent_path();
            while (!parent.empty()) {
                allowedDirs_.insert(pathUtf8(parent));
                parent = parent.parent_path();
            }
        }
    }
    ~ExtractCallback() {
        if (sink_)
            sink_->Release();
    }
    REF_METHODS(ExtractCallback)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) noexcept override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IArchiveExtractCallback || iid == IID_IProgress)
            *out = static_cast<IArchiveExtractCallback *>(this);
        else if (iid == IID_ICryptoGetTextPassword)
            *out = static_cast<ICryptoGetTextPassword *>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64 *) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32 mode) noexcept override {
        return mode == NArchive::NExtract::NAskMode::kExtract ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR *password) noexcept override {
        *password = SysAllocStringLen(password_.data(), static_cast<UINT>(password_.size()));
        return *password ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISequentialOutStream **stream,
                                        Int32 mode) noexcept override {
        *stream = nullptr;
        try {
            if (sink_) {
                sink_->Release();
                sink_ = nullptr;
            }
            if (mode != NArchive::NExtract::NAskMode::kExtract)
                return E_FAIL;
            PROPVARIANT path;
            PropVariantInit(&path);
            if (archive_->GetProperty(index, kpidPath, &path) != S_OK || path.vt != VT_BSTR) {
                PropVariantClear(&path);
                throw std::runtime_error("Archive entry has no path");
            }
            current_ = normalized(pathUtf8(fs::path(path.bstrVal)));
            PropVariantClear(&path);
            PROPVARIANT value;
            PropVariantInit(&value);
            if (archive_->GetProperty(index, kpidIsDir, &value) != S_OK)
                throw std::runtime_error("Cannot query directory");
            directory_ = value.vt == VT_BOOL && value.boolVal != VARIANT_FALSE;
            PropVariantClear(&value);
            for (auto property : {kpidSymLink, kpidHardLink, kpidIsAltStream}) {
                if (archive_->GetProperty(index, property, &value) != S_OK)
                    throw std::runtime_error("Cannot inspect link/stream property");
                bool unsupported = (value.vt == VT_BSTR && SysStringLen(value.bstrVal) != 0) ||
                                   (value.vt == VT_BOOL && value.boolVal != VARIANT_FALSE);
                PropVariantClear(&value);
                if (unsupported)
                    throw std::runtime_error("Links/alternate streams unsupported");
            }
            if (directory_) {
                if (!allowedDirs_.count(current_) || !seenDirs_.insert(current_).second)
                    throw std::runtime_error("Unexpected/duplicate directory");
                return S_OK;
            }
            auto found = files_.find(current_);
            if (found == files_.end() || !seen_.insert(current_).second)
                throw std::runtime_error("Unexpected/duplicate archive file");
            if (archive_->GetProperty(index, kpidSize, &value) != S_OK || value.vt != VT_UI8 ||
                value.uhVal.QuadPart != found->second.size) {
                PropVariantClear(&value);
                throw std::runtime_error("Archive size mismatch");
            }
            PropVariantClear(&value);
            expectedHash_ = found->second.hash;
            sink_ = new HashSink(found->second.size);
            sink_->AddRef();
            *stream = sink_;
            return S_OK;
        } catch (const std::exception &e) {
            error = e.what();
            return E_FAIL;
        } catch (...) {
            error = "Archive stream failure";
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 result) noexcept override {
        try {
            if (result != NArchive::NExtract::NOperationResult::kOK) {
                error = "Decoder operation failed: " + std::to_string(result);
                return E_FAIL;
            }
            if (!directory_) {
                if (!sink_ || sink_->written != sink_->expected ||
                    sink_->hash.finish() != expectedHash_) {
                    error = "Restored SHA-256 mismatch: " + current_;
                    return E_FAIL;
                }
                ++verified;
                bytes += sink_->written;
                sink_->Release();
                sink_ = nullptr;
            }
            return S_OK;
        } catch (...) {
            error = "Hash verification failure";
            return E_FAIL;
        }
    }
    bool complete() const {
        if (verified != files_.size())
            return false;
        for (const auto &dir : directories_)
            if (!seenDirs_.count(dir))
                return false;
        return true;
    }
};
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
struct Item {
    std::string relative;
    std::vector<unsigned char> data;
    BY_HANDLE_FILE_INFORMATION info{};
    bool directory = false;
};
class UpdateCallback final : public IArchiveUpdateCallback, public IArchiveUpdateCallbackFile,
                             public ICryptoGetTextPassword2,
                             public RefCount {
    const std::vector<Item> &items_;
    std::wstring password_;
  public:
    bool failed = false;
    UpdateCallback(const std::vector<Item> &items, const std::string &password)
        : items_(items), password_(utf8Path(password).wstring()) {}
    REF_METHODS(UpdateCallback)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) noexcept override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IProgress || iid == IID_IArchiveUpdateCallback)
            *out = static_cast<IArchiveUpdateCallback *>(this);
        else if (iid == IID_ICryptoGetTextPassword2)
            *out = static_cast<ICryptoGetTextPassword2 *>(this);
        else if (iid == IID_IArchiveUpdateCallbackFile)
            *out = static_cast<IArchiveUpdateCallbackFile *>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64 *) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetUpdateItemInfo(UInt32 index, Int32 *data, Int32 *props,
                                                UInt32 *oldIndex) noexcept override {
        if (index >= items_.size()) return E_INVALIDARG;
        *data = 1; *props = 1; *oldIndex = static_cast<UInt32>(-1); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetProperty(UInt32 index, PROPID prop, PROPVARIANT *value) noexcept override {
        PropVariantInit(value);
        if (index >= items_.size()) return E_INVALIDARG;
        try {
            const auto &item = items_[index];
            switch (prop) {
            case kpidPath: {
                auto path = utf8Path(item.relative).wstring();
                value->vt = VT_BSTR;
                value->bstrVal = SysAllocStringLen(path.data(), static_cast<UINT>(path.size()));
                return value->bstrVal ? S_OK : E_OUTOFMEMORY;
            }
            case kpidIsDir: value->vt = VT_BOOL; value->boolVal = item.directory ? VARIANT_TRUE : VARIANT_FALSE; break;
            case kpidSize: value->vt = VT_UI8; value->uhVal.QuadPart = item.data.size(); break;
            case kpidAttrib: value->vt = VT_UI4; value->ulVal = item.info.dwFileAttributes; break;
            case kpidMTime: value->vt = VT_FILETIME; value->filetime = item.info.ftLastWriteTime; break;
            case kpidCTime: value->vt = VT_FILETIME; value->filetime = item.info.ftCreationTime; break;
            case kpidATime: value->vt = VT_FILETIME; value->filetime = item.info.ftLastAccessTime; break;
            default: break;
            }
            return S_OK;
        } catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISequentialInStream **stream) noexcept override {
        *stream = nullptr;
        if (index >= items_.size()) return E_INVALIDARG;
        if (items_[index].directory) return S_OK;
        try {
            *stream = new MemoryInput(items_[index].data.data(), items_[index].data.size());
            return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 result) noexcept override {
        if (result != NArchive::NExtract::NOperationResult::kOK) { failed = true; return E_FAIL; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream2(UInt32 index, ISequentialInStream **stream,
                                         UInt32) noexcept override {
        // Match the CLI's analysis callback: executable/Delta auto-filter selection
        // must inspect real content instead of falling back to filename heuristics.
        return GetStream(index, stream);
    }
    HRESULT STDMETHODCALLTYPE ReportOperation(UInt32, UInt32, UInt32) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword2(Int32 *defined, BSTR *password) noexcept override {
        *defined = password_.empty() ? 0 : 1;
        *password = SysAllocStringLen(password_.data(), static_cast<UINT>(password_.size()));
        return *password ? S_OK : E_OUTOFMEMORY;
    }
};
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Mapping {
    void *data = nullptr;
    ~Mapping() { if (data) UnmapViewOfFile(data); }
};
struct Fallback : std::runtime_error { using std::runtime_error::runtime_error; };
std::string checkedText(const Json &value) {
    auto s = value.get<std::string>();
    if (s.find('\0') != std::string::npos) throw std::runtime_error("Embedded NUL in request");
    return s;
}
void validateSource(const fs::path &path, const fs::path &root) {
    if (!path.is_absolute() || !isWithin(path, root)) throw std::runtime_error("Input outside root");
    for (auto ancestor = path; !ancestor.empty(); ancestor = ancestor.parent_path()) {
        if (isLinkOrReparse(ancestor)) throw std::runtime_error("Input links/reparse points unsupported");
        if (ancestor == ancestor.parent_path()) break;
    }
}
Item loadItem(const fs::path &path, const fs::path &root, const std::string &relative,
              bool directory, uint64_t expectedSize, const std::string &expectedHash) {
    validateSource(path, root);
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN), nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot lock input");
    Item item;
    item.relative = relative; item.directory = directory;
    if (!GetFileInformationByHandle(file.value, &item.info) ||
        (item.info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(item.info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
        throw std::runtime_error("Input metadata mismatch");
    if (directory) return item;
    auto size = (uint64_t(item.info.nFileSizeHigh) << 32) | item.info.nFileSizeLow;
    if (size != expectedSize) throw std::runtime_error("Input size changed");
    item.data.resize(static_cast<size_t>(size));
    Hasher hash;
    uint64_t offset = 0;
    while (offset < size) {
        DWORD count = 0;
        auto n = static_cast<DWORD>(std::min<uint64_t>(1048576, size - offset));
        if (!ReadFile(file.value, item.data.data() + offset, n, &count, nullptr) || !count)
            throw std::runtime_error("Input read failed");
        hash.update(item.data.data() + offset, count); offset += count;
    }
    if (hash.finish() != expectedHash) throw std::runtime_error("Input SHA-256 changed");
    return item;
}
Item loadMemoryItem(const Json &source, const std::string &relative,
                    uint64_t expectedSize, const std::string &expectedHash) {
    if (!source.is_object()) throw std::runtime_error("Invalid RAM input source");
    auto name = checkedText(source.at("mappingName"));
    constexpr const char *prefix = "Local\\UltraArch-RAM-";
    if (name.rfind(prefix, 0) != 0 || name.size() <= std::strlen(prefix) || name.size() > 240 ||
        name.find('\\', std::strlen(prefix)) != std::string::npos || name.find('/') != std::string::npos)
        throw std::runtime_error("Invalid RAM input mapping name");
    auto capacity = source.at("capacityBytes").get<uint64_t>();
    auto offset = source.at("offset").get<uint64_t>();
    auto size = source.at("size").get<uint64_t>();
    if (!capacity || capacity > 512ULL * 1048576 || offset > capacity ||
        size > capacity - offset || size != expectedSize)
        throw std::runtime_error("Invalid RAM input range");
    Item item;
    item.relative = relative;
    item.info.dwFileAttributes = source.value("attributes", uint32_t(FILE_ATTRIBUTE_ARCHIVE));
    if (item.info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Invalid RAM input attributes");
    if (!item.info.dwFileAttributes) item.info.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    auto fileTime = [](uint64_t ticks) {
        return FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    };
    item.info.ftLastWriteTime = fileTime(source.value("mtime", uint64_t(0)));
    item.info.ftCreationTime = fileTime(source.value("ctime", uint64_t(0)));
    item.info.ftLastAccessTime = fileTime(source.value("atime", uint64_t(0)));
    item.info.nFileSizeHigh = static_cast<DWORD>(size >> 32);
    item.info.nFileSizeLow = static_cast<DWORD>(size);
    auto wideName = utf8Path(name).wstring();
    Handle mapping{OpenFileMappingW(FILE_MAP_READ, FALSE, wideName.c_str())};
    if (!mapping.value) throw std::runtime_error("Cannot open RAM input mapping");
    Mapping view{MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(capacity))};
    if (!view.data) throw std::runtime_error("Cannot map bounded RAM input");
    item.data.resize(static_cast<size_t>(size));
    Hasher hash;
    for (uint64_t done = 0; done < size;) {
        auto count = static_cast<size_t>(std::min<uint64_t>(1048576, size - done));
        std::memcpy(item.data.data() + done, static_cast<const unsigned char *>(view.data) + offset + done, count);
        hash.update(item.data.data() + done, count);
        done += count;
    }
    if (hash.finish() != expectedHash) throw std::runtime_error("RAM input SHA-256 changed");
    return item;
}
void readExact(HANDLE input, void *data, DWORD size) {
    while (size) {
        DWORD read = 0;
        if (!ReadFile(input, data, size, &read, nullptr) || !read) throw std::runtime_error("Incomplete encoder request");
        size -= read; data = static_cast<char *>(data) + read;
    }
}
void writeReply(const Json &reply) {
    auto text = reply.dump();
    if (text.size() > EncodeWorkerProtocol::maxReplyBytes) return;
    std::string frame = EncodeWorkerProtocol::magic;
    auto n = static_cast<uint32_t>(text.size());
    for (int i = 0; i < 4; ++i) frame.push_back(static_cast<char>(n >> (8 * i)));
    frame += text;
    size_t offset = 0;
    while (offset < frame.size()) {
        DWORD wrote = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), frame.data() + offset,
                       static_cast<DWORD>(frame.size() - offset), &wrote, nullptr) || !wrote) return;
        offset += wrote;
    }
}
Json run(const Json &request) {
    if (request.at("schema").get<uint32_t>() != EncodeWorkerProtocol::schema)
        throw std::runtime_error("Unknown encoder schema");
    auto capacity = request.at("capacityBytes").get<uint64_t>();
    auto limit = request.at("inputLimitBytes").get<uint64_t>();
    if (!capacity || capacity > EncodeWorkerProtocol::maxCapacityBytes || !limit || limit > EncodeWorkerProtocol::maxInputBytes)
        throw std::runtime_error("Invalid RAM quota");
    auto root = utf8Path(checkedText(request.at("root")));
    if (!root.is_absolute() || !fs::is_directory(root)) throw std::runtime_error("Invalid input root");
    validateSource(root, root);
    const auto &files = request.at("files");
    const auto &dirs = request.at("directories");
    if (!files.is_array() || !dirs.is_array() || files.size() + dirs.size() > 65536)
        throw std::runtime_error("Invalid input manifest");
    uint64_t rawBytes = 0;
    std::set<std::string> paths;
    std::vector<Item> items;
    Json manifest{{"files", Json::array()}, {"directories", Json::array()}};
    for (const auto &entry : files) {
        auto relative = normalized(checkedText(entry.at("relative")));
        if (!paths.insert(archivePathKey(relative)).second) throw std::runtime_error("Duplicate input path");
        auto size = entry.at("size").get<uint64_t>();
        if (size > limit - rawBytes) throw std::runtime_error("Input RAM quota exceeded");
        rawBytes += size;
        auto hash = checkedText(entry.at("sha256"));
        if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("Invalid input SHA-256");
        auto path = utf8Path(checkedText(entry.at("path")));
        if (entry.contains("memory")) {
            // Explicit RAM sources use logical archive paths; never probe or open a disk fallback.
            if (!path.is_absolute() || archivePathKey(normalized(pathUtf8(
                path.lexically_normal().lexically_relative(root.lexically_normal())))) != archivePathKey(relative))
                throw std::runtime_error("Relative RAM input mismatch");
            items.push_back(loadMemoryItem(entry.at("memory"), relative, size, hash));
        } else {
            if (!isWithin(path, root) || archivePathKey(pathUtf8(fs::relative(path, root))) != archivePathKey(relative))
                throw std::runtime_error("Relative input mismatch");
            items.push_back(loadItem(path, root, relative, false, size, hash));
        }
        manifest["files"].push_back({{"path", relative}, {"size", size}, {"sha256", hash}});
    }
    for (const auto &entry : dirs) {
        auto relative = normalized(checkedText(entry));
        if (!paths.insert(archivePathKey(relative)).second) throw std::runtime_error("Duplicate directory path");
        items.push_back(loadItem(root / utf8Path(relative), root, relative, true, 0, {}));
        manifest["directories"].push_back(relative);
    }
    auto dll = utf8Path(checkedText(request.at("dll")));
    if (!dll.is_absolute() || isLinkOrReparse(dll) || fileChecksum(dll) != pinned7ZipDllSha256)
        throw std::runtime_error("Encoder requires pinned official 7-Zip DLL");
    Handle dllLock{CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (dllLock.value == INVALID_HANDLE_VALUE || fileChecksum(dll) != pinned7ZipDllSha256)
        throw std::runtime_error("Cannot lock pinned DLL");
    auto mappingName = utf8Path(checkedText(request.at("mappingName"))).wstring();
    if (mappingName.empty() || mappingName.size() > 240) throw std::runtime_error("Invalid mapping name");
    Handle mapping{OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mappingName.c_str())};
    if (!mapping.value) throw std::runtime_error("Cannot open parent RAM mapping");
    Mapping view{MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(capacity))};
    if (!view.data) throw std::runtime_error("Cannot map bounded output");
    struct Module { HMODULE value; ~Module() { if (value) FreeLibrary(value); } } module{
        LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!module.value) throw std::runtime_error("Cannot load encoder DLL");
    using Create = HRESULT(WINAPI *)(const GUID *, const GUID *, void **);
    auto create = reinterpret_cast<Create>(GetProcAddress(module.value, "CreateObject"));
    Owner<IOutArchive> encoder;
    if (!create || create(&format7z, &IID_IOutArchive, reinterpret_cast<void **>(&encoder.p)) != S_OK)
        throw std::runtime_error("Cannot create encoder");
    Owner<ISetProperties> setter;
    if (encoder->QueryInterface(IID_ISetProperties, reinterpret_cast<void **>(&setter.p)) != S_OK)
        throw Fallback("Backend properties unsupported");
    const auto &properties = request.at("properties");
    if (!properties.is_array() || properties.empty() || properties.size() > 128)
        throw std::runtime_error("Invalid encoder properties");
    std::vector<std::wstring> names, values;
    std::set<std::wstring> seenProperties;
    for (const auto &property : properties) {
        auto name = utf8Path(checkedText(property.at("name"))).wstring();
        auto value = utf8Path(checkedText(property.at("value"))).wstring();
        if (name.empty() || name.size() > 128 || value.size() > 4096 || !seenProperties.insert(name).second)
            throw std::runtime_error("Invalid/duplicate encoder property");
        names.push_back(std::move(name)); values.push_back(std::move(value));
    }
    std::vector<const wchar_t *> namePointers;
    std::vector<PROPVARIANT> variants(values.size());
    struct Variants { std::vector<PROPVARIANT> &values; ~Variants() { for (auto &v : values) PropVariantClear(&v); } } variantsOwner{variants};
    for (size_t i = 0; i < values.size(); ++i) {
        namePointers.push_back(names[i].c_str());
        PropVariantInit(&variants[i]);
        auto key = names[i];
        for (auto &ch : key) if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
        if (key.size() > 1 && key.front() == L'b' && key.find(L':') != std::wstring::npos) {
            // Official 7-Zip bindings require VT_EMPTY, not an empty BSTR.
            if (!values[i].empty()) throw std::runtime_error("Binding value must be empty");
        } else if (key == L"x") {
            // The backend level parser accepts VT_UI4 or a number in the name, not BSTR.
            if (values[i].empty() || values[i].find_first_not_of(L"0123456789") != std::wstring::npos)
                throw std::runtime_error("Invalid compression level");
            auto level = std::stoull(values[i]);
            if (level > 9) throw std::runtime_error("Compression level outside supported range");
            variants[i].vt = VT_UI4; variants[i].ulVal = static_cast<ULONG>(level);
        } else {
            variants[i].vt = VT_BSTR;
            variants[i].bstrVal = SysAllocStringLen(values[i].data(), static_cast<UINT>(values[i].size()));
            if (!variants[i].bstrVal) throw std::bad_alloc();
        }
    }
    auto status = setter->SetProperties(namePointers.data(), variants.data(), static_cast<UInt32>(variants.size()));
    if (status == E_OUTOFMEMORY) throw std::bad_alloc();
    if (status == E_INVALIDARG || status == E_NOTIMPL) throw Fallback("Encoder property/method unsupported");
    if (status != S_OK) throw std::runtime_error("Encoder property failure");
    auto password = request.contains("password") ? checkedText(request.at("password")) : std::string{};
    Owner<MappingOutput> output{new MappingOutput(static_cast<unsigned char *>(view.data), capacity)};
    Owner<UpdateCallback> update{new UpdateCallback(items, password)};
    status = encoder->UpdateItems(output.p, static_cast<UInt32>(items.size()), update.p);
    if (status == E_OUTOFMEMORY) throw std::bad_alloc();
    if (output->overflow) throw Fallback("RAM output capacity exceeded");
    if (status == E_NOTIMPL) throw Fallback("Encoder method unsupported");
    if (status != S_OK || update->failed || !output->bytes) throw std::runtime_error("RAM encoding failed");
    // Encoding and verification share the reserved codec-memory slot. Release
    // every encoder interface before constructing the decoder, including setter's
    // reference to the same COM object, so retained buffers cannot overlap peaks.
    setter.p->Release(); setter.p = nullptr;
    encoder.p->Release(); encoder.p = nullptr;
    Owner<IInArchive> decoder;
    if (create(&format7z, &IID_IInArchive, reinterpret_cast<void **>(&decoder.p)) != S_OK)
        throw std::runtime_error("Cannot create verification decoder");
    limitDecoderThreads(decoder.p);
    Owner<MemoryInput> input{new MemoryInput(static_cast<const unsigned char *>(view.data), output->bytes)};
    Owner<OpenCallback> open{new OpenCallback(password)};
    UInt64 scanLimit = 0;
    status = decoder->Open(input.p, &scanLimit, open.p);
    if (status == E_OUTOFMEMORY) throw std::bad_alloc();
    if (status != S_OK) throw std::runtime_error("RAM archive header verification failed");
    Owner<ExtractCallback> verify{new ExtractCallback(decoder.p, manifest, password)};
    status = decoder->Extract(nullptr, static_cast<UInt32>(-1), 0, verify.p);
    decoder->Close();
    if (status == E_OUTOFMEMORY) throw std::bad_alloc();
    if (status != S_OK || !verify->error.empty() || !verify->complete())
        throw std::runtime_error(verify->error.empty() ? "Incomplete RAM verification" : verify->error);
    return {{"schema", 1}, {"success", true}, {"verified", true}, {"fallback", false},
            {"files", verify->verified}, {"rawBytes", rawBytes}, {"bytes", output->bytes}, {"error", ""}};
}
} // namespace
int main() {
    try {
        char header[8];
        readExact(GetStdHandle(STD_INPUT_HANDLE), header, 8);
        if (std::string(header, 4) != EncodeWorkerProtocol::magic) throw std::runtime_error("Invalid encoder protocol");
        uint32_t length = 0;
        for (int i = 0; i < 4; ++i) length |= uint32_t(static_cast<unsigned char>(header[4 + i])) << (8 * i);
        if (!length || length > EncodeWorkerProtocol::maxRequestBytes) throw std::runtime_error("Encoder request too large");
        std::string request(length, '\0');
        readExact(GetStdHandle(STD_INPUT_HANDLE), request.data(), length);
        writeReply(run(Json::parse(request))); return 0;
    } catch (const std::bad_alloc &) {
        try {
            writeReply({{"schema", 1}, {"success", false}, {"verified", false}, {"fallback", false},
                        {"bytes", 0}, {"files", 0}, {"rawBytes", 0}, {"error", "Out of memory"}});
        } catch (...) {}
        return 8;
    } catch (const std::exception &e) {
        auto error = std::string(e.what()).substr(0, 4096);
        writeReply({{"schema", 1}, {"success", false}, {"verified", false},
                    {"fallback", dynamic_cast<const Fallback *>(&e) != nullptr},
                    {"bytes", 0}, {"files", 0}, {"rawBytes", 0}, {"error", error}});
        return 1;
    } catch (...) {
        writeReply({{"schema", 1}, {"success", false}, {"verified", false}, {"fallback", false},
                    {"bytes", 0}, {"files", 0}, {"rawBytes", 0}, {"error", "Encoder worker failure"}});
        return 1;
    }
}
