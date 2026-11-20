#pragma once

#include "ContentAnalyzer.hpp"
#include "Types.hpp"
#include <queue>
#include <string>
#include <vector>

namespace UltraArch {

class TaskMatrixGenerator {
  public:
    TaskMatrixGenerator() = default;

    // Generates a tailored test queue based on the analyzed content and selected settings
    std::vector<TaskConfig> generateMatrix(const AnalysisSummary &summary,
                                           const OptimizerSettings &settings);

    // Calculates estimated memory requirement in MB for given 7z parameters
    static uint64_t estimateMemoryMB(const std::vector<std::string> &flags, bool isCompress = true);
};

} // namespace UltraArch
