#pragma once
#include <cstdint>
#include <memory>
#include <string>
namespace UltraArch {
// Explicit content source. path remains an archive name; it need not be a file
// while memory is present. owner keeps the immutable mapping alive through use.
struct MemoryContent {
    std::shared_ptr<void> owner;
    const uint8_t *data = nullptr;
    std::string mappingName;
    uint64_t offset = 0, size = 0, capacity = 0;
    uint64_t mtime = 0, ctime = 0, atime = 0;
    uint32_t attributes = 32;
};
} // namespace UltraArch
