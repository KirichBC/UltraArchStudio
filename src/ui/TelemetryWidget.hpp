#pragma once

#include "../core/Types.hpp"
#include <vector>
#include <array>
#include <chrono>

namespace UltraArch {

class TelemetryWidget {
public:
    TelemetryWidget();

    // Updates live CPU, RAM, and I/O metrics
    void update(uint64_t ramLimitMB);

    // Renders the telemetry graphs panel
    void render();

private:
    static constexpr size_t HISTORY_CAPACITY = 60;
    size_t m_historyIndex = 0;
    std::array<float, HISTORY_CAPACITY> m_cpuRing = {};
    std::array<float, HISTORY_CAPACITY> m_ramRing = {};
    std::array<float, HISTORY_CAPACITY> m_diskRing = {};
    size_t m_sampleCount = 0;

    float m_currentCpu = 0.0f;
    uint64_t m_currentRamMB = 0;
    uint64_t m_totalRamMB = 0;
    uint64_t m_ramLimitMB = 8192;
    float m_currentDiskMBs = 0.0f;

    std::chrono::steady_clock::time_point m_lastUpdate;

    uint64_t m_prevIdleTime = 0;
    uint64_t m_prevKernelTime = 0;
    uint64_t m_prevUserTime = 0;
    uint64_t m_prevReadTransfer = 0;
    uint64_t m_prevWriteTransfer = 0;
};

} // namespace UltraArch
