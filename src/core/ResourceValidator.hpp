#pragma once
#include "FileUtils.hpp"
namespace UltraArch {
// Changed static PNG/JPEG files require equal pixels and preserved color/orientation metadata.
// Animated and other changed formats are rejected until dedicated validators exist.
class ResourceValidator {
  public:
    static bool equivalent(const std::filesystem::path &original,
                           const std::filesystem::path &proposed);
};
} // namespace UltraArch
