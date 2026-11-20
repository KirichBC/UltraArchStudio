#pragma once
#include "EmbeddedTools.hpp"
#include "Types.hpp"
#include <mutex>
#include <nlohmann/json.hpp>
namespace UltraArch {
struct StructuredError {
    EngineState phase = EngineState::Idle;
    ErrorCode category = ErrorCode::None;
    std::string tool, message;
    int exitCode = 0;
    uint32_t win32Error = 0;
};
class Diagnostics {
  public:
    static std::string redact(std::string text, const std::string &secret) {
        if (!secret.empty()) {
            size_t pos = 0;
            while ((pos = text.find(secret, pos)) != std::string::npos) {
                text.replace(pos, secret.size(), "<redacted>");
                pos += 10;
            }
        }
        return text;
    }
    static ErrorCode classify(const std::exception &error) {
        if (auto fileError = dynamic_cast<const std::filesystem::filesystem_error *>(&error)) {
            auto code = fileError->code();
            if (code == std::errc::no_space_on_device)
                return ErrorCode::DiskFull;
            if (code == std::errc::permission_denied || code == std::errc::operation_not_permitted)
                return ErrorCode::PermissionDenied;
            if (code == std::errc::no_such_file_or_directory)
                return ErrorCode::ToolNotFound;
        }
        if (dynamic_cast<const std::bad_alloc *>(&error))
            return ErrorCode::OutOfMemory;
        return ErrorCode::Unknown;
    }
    static std::filesystem::path logDirectory() {
        auto dir = EmbeddedTools::getDataDirectory() / "logs";
        std::filesystem::create_directories(dir);
        return dir;
    }
};
} // namespace UltraArch
