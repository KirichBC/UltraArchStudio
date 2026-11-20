#include "TelemetryWidget.hpp"
#include "../core/ProcessManager.hpp"
#include "Localization.hpp"
#include "Theme.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifdef __APPLE__
#include <mach/mach_error.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/vm_map.h>
#endif

#ifdef __linux__
#include <fstream>
#include <string>
#endif

namespace UltraArch {

TelemetryWidget::TelemetryWidget() {
    m_lastUpdate = std::chrono::steady_clock::now();
}

void TelemetryWidget::update(uint64_t ramLimitMB) {
    m_ramLimitMB = ramLimitMB;
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastUpdate).count();
    if (elapsedMs < 500) {
        return;
    }
    m_lastUpdate = now;

#ifdef _WIN32
    // 1. RAM Usage
    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        m_totalRamMB = memStatus.ullTotalPhys / (1024 * 1024);
        m_currentRamMB = (memStatus.ullTotalPhys - memStatus.ullAvailPhys) / (1024 * 1024);
    }

    // 2. CPU Usage
    FILETIME idleTime, kernelTime, userTime;
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        auto ftToUint64 = [](const FILETIME &ft) -> uint64_t {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };

        uint64_t idle = ftToUint64(idleTime);
        uint64_t kernel = ftToUint64(kernelTime);
        uint64_t user = ftToUint64(userTime);

        if (m_prevKernelTime > 0) {
            uint64_t kernelDiff = kernel - m_prevKernelTime;
            uint64_t userDiff = user - m_prevUserTime;
            uint64_t idleDiff = idle - m_prevIdleTime;
            uint64_t total = kernelDiff + userDiff;

            if (total > 0) {
                m_currentCpu =
                    static_cast<float>(total - idleDiff) * 100.0f / static_cast<float>(total);
                if (m_currentCpu < 0.0f)
                    m_currentCpu = 0.0f;
                if (m_currentCpu > 100.0f)
                    m_currentCpu = 100.0f;
            }
        }

        m_prevIdleTime = idle;
        m_prevKernelTime = kernel;
        m_prevUserTime = user;
    }

    // Committed memory and I/O of active child compressors, including completed I/O totals.
    auto children = ProcessManager::telemetry();
    m_currentRamMB = children.memoryMB;
    if (children.readBytes >= m_prevReadTransfer && children.writeBytes >= m_prevWriteTransfer) {
        m_currentDiskMBs = static_cast<float>((children.readBytes - m_prevReadTransfer) +
                                              (children.writeBytes - m_prevWriteTransfer)) /
                           (1024.0f * 1024.0f) / (elapsedMs / 1000.0f);
    }
    m_prevReadTransfer = children.readBytes;
    m_prevWriteTransfer = children.writeBytes;
#elif defined(__APPLE__)
    vm_size_t page_size;
    mach_port_t mach_port;
    mach_msg_type_number_t count;
    vm_statistics64_data_t vm_stats;

    mach_port = mach_host_self();
    count = sizeof(vm_stats) / sizeof(natural_t);
    if (KERN_SUCCESS == host_page_size(mach_port, &page_size) &&
        KERN_SUCCESS ==
            host_statistics64(mach_port, HOST_VM_INFO64, (host_info64_t)&vm_stats, &count)) {
        long long free_memory = (int64_t)vm_stats.free_count * (int64_t)page_size;
        long long used_memory = ((int64_t)vm_stats.active_count + (int64_t)vm_stats.inactive_count +
                                 (int64_t)vm_stats.wire_count) *
                                (int64_t)page_size;
        m_totalRamMB = (free_memory + used_memory) / (1024 * 1024);
        m_currentRamMB = used_memory / (1024 * 1024);
    }

    natural_t cpuCount;
    processor_info_array_t infoArray;
    mach_msg_type_number_t infoCount;
    if (host_processor_info(mach_port, PROCESSOR_CPU_LOAD_INFO, &cpuCount, &infoArray,
                            &infoCount) == KERN_SUCCESS) {
        uint64_t totalUser = 0, totalSystem = 0, totalIdle = 0;
        for (natural_t i = 0; i < cpuCount; i++) {
            totalUser += infoArray[i * CPU_STATE_MAX + CPU_STATE_USER];
            totalSystem += infoArray[i * CPU_STATE_MAX + CPU_STATE_SYSTEM];
            totalIdle += infoArray[i * CPU_STATE_MAX + CPU_STATE_IDLE];
        }
        vm_deallocate(mach_task_self(), (vm_address_t)infoArray, infoCount * sizeof(integer_t));

        if (m_prevCpuUser > 0) {
            uint64_t diffUser = totalUser - m_prevCpuUser;
            uint64_t diffSystem = totalSystem - m_prevCpuSystem;
            uint64_t diffIdle = totalIdle - m_prevCpuIdle;
            uint64_t total = diffUser + diffSystem + diffIdle;
            if (total > 0) {
                m_currentCpu =
                    static_cast<float>(diffUser + diffSystem) * 100.0f / static_cast<float>(total);
            }
        }
        m_prevCpuUser = totalUser;
        m_prevCpuSystem = totalSystem;
        m_prevCpuIdle = totalIdle;
    }

    // Simulate disk for now
    m_currentDiskMBs = 0.0f;
#else
    // Fallback Linux implementation
    m_totalRamMB = 0;
    m_currentRamMB = 0;
    m_currentCpu = 0.0f;
    m_currentDiskMBs = 0.0f;
#endif

    // Update ring buffers
    m_cpuRing[m_historyIndex] = m_currentCpu;
    m_ramRing[m_historyIndex] = static_cast<float>(m_currentRamMB);
    m_diskRing[m_historyIndex] = m_currentDiskMBs;
    m_historyIndex = (m_historyIndex + 1) % HISTORY_CAPACITY;
    if (m_sampleCount < HISTORY_CAPACITY)
        m_sampleCount++;
}

void TelemetryWidget::render() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::CardBg);
    ImGui::BeginChild("TelemetryPanel", ImVec2(0, 110 * Theme::getDpiScale()), true,
                      ImGuiWindowFlags_NoScrollbar);

    if (ImGui::BeginTable("TelemetryColumns", 3)) {
        ImGui::TableNextRow();

        // Prepare linear arrays for plots
        std::vector<float> cpuPlot(HISTORY_CAPACITY, 0.0f);
        std::vector<float> ramPlot(HISTORY_CAPACITY, 0.0f);
        std::vector<float> diskPlot(HISTORY_CAPACITY, 0.0f);

        size_t count = m_sampleCount;
        for (size_t i = 0; i < count; ++i) {
            size_t idx = (m_historyIndex + HISTORY_CAPACITY - count + i) % HISTORY_CAPACITY;
            cpuPlot[i] = m_cpuRing[idx];
            ramPlot[i] = m_ramRing[idx];
            diskPlot[i] = m_diskRing[idx];
        }
        int plotCount = static_cast<int>(std::max<size_t>(count, 2));

        // Column 1: CPU Load
        ImGui::TableNextColumn();
        {
            std::stringstream ss;
            ss << std::fixed << std::setprecision(1) << m_currentCpu << "%";
            ImGui::TextColored(Theme::Colors::TextSecondary, "%s", L::tr(StringId::CpuLoad));
            ImGui::SameLine();
            ImGui::TextColored(Theme::Colors::Accent, "%s", ss.str().c_str());

            ImGui::PlotLines("##CpuPlot", cpuPlot.data(), plotCount, 0, nullptr, 0.0f, 100.0f,
                             ImVec2(-1, 55 * Theme::getDpiScale()));
        }

        // Column 2: Physical RAM & Job Limit
        ImGui::TableNextColumn();
        {
            std::stringstream ss;
            ss << m_currentRamMB << " / " << m_ramLimitMB << " MiB (child commit / limit)";
            ImGui::TextColored(Theme::Colors::TextSecondary, "%s", L::tr(StringId::RamUsage));
            ImGui::SameLine();
            if (m_currentRamMB > m_ramLimitMB) {
                ImGui::TextColored(Theme::Colors::ErrorRed, "%s", ss.str().c_str());
            } else {
                ImGui::TextColored(Theme::Colors::LeaderGreen, "%s", ss.str().c_str());
            }

            float maxRamPlot = static_cast<float>(std::max(m_ramLimitMB, m_totalRamMB));
            ImGui::PlotLines("##RamPlot", ramPlot.data(), plotCount, 0, nullptr, 0.0f, maxRamPlot,
                             ImVec2(-1, 55 * Theme::getDpiScale()));
        }

        // Column 3: Disk I/O Activity
        ImGui::TableNextColumn();
        {
            std::stringstream ss;
            ss << std::fixed << std::setprecision(1) << m_currentDiskMBs << " MB/s";
            ImGui::TextColored(Theme::Colors::TextSecondary, "%s", L::tr(StringId::DiskIO));
            ImGui::SameLine();
            ImGui::TextColored(Theme::Colors::TextBright, "%s", ss.str().c_str());

            float maxDisk = 100.0f;
            for (size_t i = 0; i < count; ++i) {
                if (diskPlot[i] * 1.5f > maxDisk) {
                    maxDisk = diskPlot[i] * 1.5f;
                }
            }

            ImGui::PlotLines("##DiskPlot", diskPlot.data(), plotCount, 0, nullptr, 0.0f, maxDisk,
                             ImVec2(-1, 55 * Theme::getDpiScale()));
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace UltraArch
