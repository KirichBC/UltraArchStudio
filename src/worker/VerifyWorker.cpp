// Isolated 7-Zip COM decoder. No restored file bytes are written to scratch.
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
class FileStream final : public IInStream, public RefCount {
    struct Volume {
        HANDLE handle;
        uint64_t size;
    };
    std::vector<Volume> volumes_;
    uint64_t position_ = 0, total_ = 0;

  public:
    explicit FileStream(const fs::path &archive) {
        std::vector<fs::path> paths{archive};
        auto text = pathUtf8(archive);
        if (text.size() > 4 && text.substr(text.size() - 4) == ".001") {
            for (unsigned n = 2; n < 10000; ++n) {
                std::ostringstream suffix;
                suffix << std::setw(3) << std::setfill('0') << n;
                auto next = utf8Path(text.substr(0, text.size() - 3) + suffix.str());
                if (!fs::exists(next))
                    break;
                paths.push_back(next);
            }
        }
        try {
            for (const auto &path : paths) {
                if (isLinkOrReparse(path))
                    throw std::runtime_error("Volume links unsupported");
                auto h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    throw std::runtime_error("Cannot open archive volume");
                LARGE_INTEGER size{};
                if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) {
                    CloseHandle(h);
                    throw std::runtime_error("Cannot read volume size");
                }
                volumes_.push_back({h, static_cast<uint64_t>(size.QuadPart)});
                total_ += size.QuadPart;
            }
        } catch (...) {
            for (auto v : volumes_)
                CloseHandle(v.handle);
            throw;
        }
    }
    ~FileStream() {
        for (auto v : volumes_)
            CloseHandle(v.handle);
    }
    REF_METHODS(FileStream)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) noexcept override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IInStream || iid == IID_ISequentialInStream) {
            *out = static_cast<IInStream *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Read(void *data, UInt32 size, UInt32 *processed) noexcept override {
        if (processed)
            *processed = 0;
        uint64_t base = 0;
        UInt32 done = 0;
        for (const auto &volume : volumes_) {
            if (position_ >= base + volume.size) {
                base += volume.size;
                continue;
            }
            LARGE_INTEGER offset{};
            offset.QuadPart = static_cast<LONGLONG>(position_ - base);
            if (!SetFilePointerEx(volume.handle, offset, nullptr, FILE_BEGIN))
                return HRESULT_FROM_WIN32(GetLastError());
            DWORD n = 0;
            auto count =
                static_cast<DWORD>(std::min<uint64_t>(size - done, volume.size - offset.QuadPart));
            if (!ReadFile(volume.handle, static_cast<char *>(data) + done, count, &n, nullptr))
                return HRESULT_FROM_WIN32(GetLastError());
            position_ += n;
            done += n;
            base += volume.size;
            if (done == size || n < count)
                break;
        }
        if (processed)
            *processed = done;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64 *result) noexcept override {
        uint64_t base = origin == STREAM_SEEK_SET   ? 0
                        : origin == STREAM_SEEK_CUR ? position_
                                                    : total_;
        if (origin > STREAM_SEEK_END ||
            (offset >= 0 && static_cast<uint64_t>(offset) > UINT64_MAX - base) ||
            (offset < 0 && static_cast<uint64_t>(-(offset + 1)) + 1 > base))
            return STG_E_INVALIDFUNCTION;
        position_ = offset < 0 ? base - (static_cast<uint64_t>(-(offset + 1)) + 1)
                               : base + static_cast<uint64_t>(offset);
        if (result)
            *result = position_;
        return S_OK;
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
#ifdef ULTRAARCH_FAULT_INJECTION
            // Fault builds can hold a real decoder write for deterministic cancellation.
            static std::atomic<bool> reached{false};
            if (size && !reached.exchange(true)) {
                wchar_t name[256]{};
                const auto length = GetEnvironmentVariableW(L"ULTRAARCH_VERIFY_BARRIER", name, 256);
                if (length && length < 256) {
                    const auto base = std::wstring(name);
                    HANDLE entered = OpenEventW(EVENT_MODIFY_STATE, FALSE, (base + L"-entered").c_str());
                    HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, (base + L"-release").c_str());
                    if (entered && release) {
                        SetEvent(entered);
                        WaitForSingleObject(release, 10000);
                    }
                    if (entered) CloseHandle(entered);
                    if (release) CloseHandle(release);
                }
            }
#endif
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
void readExact(HANDLE input, void *data, DWORD size) {
    while (size) {
        DWORD read = 0;
        if (!ReadFile(input, data, size, &read, nullptr) || !read)
            throw std::runtime_error("Incomplete worker request");
        size -= read;
        data = static_cast<char *>(data) + read;
    }
}
void writeReply(const Json &reply) {
    auto text = reply.dump();
    std::string frame = "UAV1";
    uint32_t n = static_cast<uint32_t>(text.size());
    for (int i = 0; i < 4; ++i)
        frame.push_back(static_cast<char>(n >> (8 * i)));
    frame += text;
    size_t offset = 0;
    while (offset < frame.size()) {
        DWORD wrote = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), frame.data() + offset,
                       static_cast<DWORD>(frame.size() - offset), &wrote, nullptr) ||
            !wrote)
            break;
        offset += wrote;
    }
}
} // namespace
int main() {
    try {
        char header[8];
        readExact(GetStdHandle(STD_INPUT_HANDLE), header, 8);
        if (std::string(header, 4) != "UAV1")
            throw std::runtime_error("Invalid worker protocol");
        uint32_t length = 0;
        for (int i = 0; i < 4; ++i)
            length |= uint32_t(static_cast<unsigned char>(header[4 + i])) << (8 * i);
        if (length > 65536)
            throw std::runtime_error("Worker request too large");
        std::string request(length, '\0');
        readExact(GetStdHandle(STD_INPUT_HANDLE), request.data(), length);
        auto json = Json::parse(request);
        auto dll = utf8Path(json.at("dll").get<std::string>());
        if (fileChecksum(dll) != pinned7ZipDllSha256)
            throw std::runtime_error(std::string("Worker requires pinned official 7-Zip ") + pinned7ZipVersion + " DLL");
        auto manifestPath = utf8Path(json.at("manifest").get<std::string>());
        if (fs::file_size(manifestPath) > 64 * 1048576 ||
            fileChecksum(manifestPath) != json.at("manifestHash").get<std::string>())
            throw std::runtime_error("Manifest identity mismatch");
        Json manifest;
        std::ifstream in(manifestPath);
        in >> manifest;
        auto module = LoadLibraryExW(
            dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            throw std::runtime_error("Cannot load decoder DLL");
        struct Module {
            HMODULE h;
            ~Module() {
                FreeLibrary(h);
            }
        } moduleOwner{module};
        using Create = HRESULT(WINAPI *)(const GUID *, const GUID *, void **);
        auto create = reinterpret_cast<Create>(GetProcAddress(module, "CreateObject"));
        Owner<IInArchive> archive;
        if (!create ||
            create(&format7z, &IID_IInArchive, reinterpret_cast<void **>(&archive.p)) != S_OK)
            throw std::runtime_error("Cannot create 7z decoder");
        limitDecoderThreads(archive.p);
        auto password = json.value("password", std::string{});
        Owner<FileStream> stream{new FileStream(utf8Path(json.at("archive").get<std::string>()))};
        Owner<OpenCallback> open{new OpenCallback(password)};
        UInt64 scanLimit = 16 * 1048576;
        if (archive->Open(stream.p, &scanLimit, open.p) != S_OK)
            throw std::runtime_error("Cannot open archive (password/header/volumes)");
        Owner<ExtractCallback> callback{new ExtractCallback(archive.p, manifest, password)};
        auto status = archive->Extract(nullptr, static_cast<UInt32>(-1), 0, callback.p);
        archive->Close();
        if (status != S_OK || !callback->error.empty() || !callback->complete())
            throw std::runtime_error(callback->error.empty() ? "Incomplete archive extraction"
                                                             : callback->error);
        writeReply({{"schema", 1},
                    {"success", true},
                    {"files", callback->verified},
                    {"bytes", callback->bytes}});
        return 0;
    } catch (const std::exception &e) {
        writeReply({{"schema", 1}, {"success", false}, {"error", e.what()}});
        return 1;
    }
}
