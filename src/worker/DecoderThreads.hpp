#pragma once
#include "7zip/CPP/7zip/Archive/IArchive.h"
#include <stdexcept>

namespace UltraArch {
// Verification/extraction owns at least one CPU token. Do not inherit the DLL's automatic thread count.
inline void limitDecoderThreads(IInArchive *archive) {
    ISetProperties *properties = nullptr;
    if (archive->QueryInterface(IID_ISetProperties, reinterpret_cast<void **>(&properties)) != S_OK)
        throw std::runtime_error("Cannot configure decoder CPU budget");
    const wchar_t *name = L"mt";
    PROPVARIANT value{};
    value.vt = VT_UI4;
    value.ulVal = 1;
    const auto status = properties->SetProperties(&name, &value, 1);
    properties->Release();
    if (status != S_OK)
        throw std::runtime_error("Decoder rejected CPU budget");
}
}
