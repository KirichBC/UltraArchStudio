#include "ResourceValidator.hpp"
#ifdef _WIN32
#include <wincodec.h>
// Small COM owner; compatible with both MSVC and MinGW.
#include <utility>
template <class T> class ComPtr {
    T *value_ = nullptr;

  public:
    ~ComPtr() {
        if (value_)
            value_->Release();
    }
    T *Get() const {
        return value_;
    }
    T **GetAddressOf() {
        return &value_;
    }
    T *operator->() const {
        return value_;
    }
};
#endif
namespace UltraArch {
#ifdef _WIN32
namespace {
// Preserve color interpretation, orientation and animation metadata. Other formats
// need a dedicated semantic validator before changed bytes can be accepted.
bool pngSemantics(const std::filesystem::path &path, std::string &digest) {
    std::ifstream in(path, std::ios::binary);
    unsigned char signature[8];
    if (!in.read(reinterpret_cast<char *>(signature), 8) ||
        std::string(reinterpret_cast<char *>(signature), 8) != std::string("\x89PNG\r\n\x1a\n", 8))
        return false;
    Hasher hash;
    bool ended = false;
    while (in && !ended) {
        unsigned char header[8];
        if (!in.read(reinterpret_cast<char *>(header), 8))
            return false;
        uint32_t size = (uint32_t(header[0]) << 24) | (uint32_t(header[1]) << 16) |
                        (uint32_t(header[2]) << 8) | header[3];
        if (size > 64 * 1048576)
            return false;
        std::string type(reinterpret_cast<char *>(header + 4), 4);
        if (type == "acTL" || type == "fcTL" || type == "fdAT")
            return false;
        bool preserve = type == "iCCP" || type == "sRGB" || type == "gAMA" || type == "cHRM" ||
                        type == "eXIf" || type == "cICP" || type == "mDCv" || type == "cLLi";
        if (preserve)
            hash.update(header, 8);
        char buffer[65536];
        uint32_t left = size;
        while (left) {
            auto count = std::min<uint32_t>(left, sizeof(buffer));
            if (!in.read(buffer, count))
                return false;
            if (preserve)
                hash.update(buffer, count);
            left -= count;
        }
        char crc[4];
        if (!in.read(crc, 4))
            return false;
        ended = type == "IEND";
    }
    if (!ended)
        return false;
    digest = hash.finish();
    return true;
}
bool imageSemantics(const std::filesystem::path &path, std::string &digest) {
    if (pngSemantics(path, digest)) {
        digest = "PNG:" + digest;
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    unsigned char magic[2];
    if (!in.read(reinterpret_cast<char *>(magic), 2) || magic[0] != 0xff || magic[1] != 0xd8)
        return false;
    Hasher hash;
    for (size_t segments = 0; segments < 100000; ++segments) {
        unsigned char marker = 0, code = 0;
        if (!in.read(reinterpret_cast<char *>(&marker), 1) || marker != 0xff)
            return false;
        do {
            if (!in.read(reinterpret_cast<char *>(&code), 1))
                return false;
        } while (code == 0xff);
        if (code == 0xda) {
            digest = "JPEG:" + hash.finish();
            return true;
        }
        if (code == 0xd9 || code == 0x00 || code == 0xd8)
            return false;
        if (code == 0x01 || (code >= 0xd0 && code <= 0xd7))
            continue;
        unsigned char length[2];
        if (!in.read(reinterpret_cast<char *>(length), 2))
            return false;
        uint32_t size = (uint32_t(length[0]) << 8) | length[1];
        if (size < 2)
            return false;
        bool preserve = code == 0xe0 || code == 0xe1 || code == 0xe2 || code == 0xee;
        if (preserve) {
            hash.update(&code, 1);
            hash.update(length, 2);
        }
        std::vector<char> bytes(size - 2);
        if (!in.read(bytes.data(), bytes.size()))
            return false;
        if (preserve)
            hash.update(bytes.data(), bytes.size());
    }
    return false;
}

} // namespace
#endif

bool ResourceValidator::equivalent(const std::filesystem::path &original,
                                   const std::filesystem::path &proposed) {
    if (fileChecksum(original) == fileChecksum(proposed))
        return true;
#ifdef _WIN32
    std::string semanticsA, semanticsB;
    if (!imageSemantics(original, semanticsA) || !imageSemantics(proposed, semanticsB) ||
        semanticsA != semanticsB)
        return false;
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComCleanup {
        HRESULT init;
        ~ComCleanup() {
            if (SUCCEEDED(init))
                CoUninitialize();
        }
    } cleanup{init};
    if (FAILED(init) && init != RPC_E_CHANGED_MODE)
        return false;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf()))))
        return false;
    auto pixels = [&](const std::filesystem::path &path, std::string &digest) {
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand,
                                                      decoder.GetAddressOf())))
            return false;
        UINT count = 0;
        if (FAILED(decoder->GetFrameCount(&count)) || count != 1)
            return false;
        Hasher hash;
        hash.update(&count, sizeof(count));
        for (UINT n = 0; n < count; ++n) {
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            UINT width = 0, height = 0;
            if (FAILED(decoder->GetFrame(n, frame.GetAddressOf())) ||
                FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
                uint64_t(width) * height > 16777216)
                return false;
            if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
                FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat64bppRGBA,
                                             WICBitmapDitherTypeNone, nullptr, 0,
                                             WICBitmapPaletteTypeCustom)))
                return false;
            hash.update(&width, sizeof(width));
            hash.update(&height, sizeof(height));
            if (width > UINT_MAX / 8)
                return false;
            std::vector<unsigned char> row(static_cast<size_t>(width) * 8);
            for (UINT y = 0; y < height; ++y) {
                WICRect rect{0, static_cast<INT>(y), static_cast<INT>(width), 1};
                if (FAILED(converter->CopyPixels(&rect, width * 8, width * 8, row.data())))
                    return false;
                hash.update(row.data(), row.size());
            }
        }
        digest = hash.finish();
        return true;
    };
    std::string a, b;
    return pixels(original, a) && pixels(proposed, b) && a == b;
#else
    // Native harness has no WIC. Never accept altered resources without a semantic validator.
    return false;
#endif
}
} // namespace UltraArch
