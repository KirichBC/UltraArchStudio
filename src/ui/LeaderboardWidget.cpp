#include "LeaderboardWidget.hpp"
#include "../core/SettingsJson.hpp"
#include "Localization.hpp"
#include "Theme.hpp"
#include "imgui.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace UltraArch {
namespace {
const char *localized(const char *en, const char *ru) {
    return L::currentLang() ? ru : en;
}
} // namespace

static std::string formatBytes(uint64_t bytes) {
    if (bytes == 0)
        return "-";
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        ss << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GiB";
    } else if (bytes >= 1024ULL * 1024ULL) {
        ss << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MiB";
    } else if (bytes >= 1024ULL) {
        ss << (static_cast<double>(bytes) / 1024.0) << " KiB";
    } else {
        ss << bytes << " B";
    }
    return ss.str();
}

void LeaderboardWidget::exportToCsv(const std::vector<CandidateResult> &results,
                                    const std::filesystem::path &path) {
    std::ofstream ofs(path);
    if (!ofs)
        return;
    auto quote = [](const std::string &value) {
        std::string result = "\"";
        for (char c : value) {
            if (c == '\"')
                result += '\"';
            result += c;
        }
        return result + '\"';
    };
    ofs << "ID;Preset;Flags;OutputBytes;Ratio;Duration;PeakRamMB;Status;Storage;StorageFallback;RamBufferBytes;LogicalTemporaryFileBytes\n";
    for (const auto &r : results) {
        ofs << r.taskId << ";" << quote(r.name) << ";" << quote(r.flagsStr) << ";" << r.outputBytes
            << ";" << r.compressionRatio << ";" << r.durationSeconds << ";" << r.peakRamMB << ";"
            << (int)r.status << ";"
            << quote(r.status == CandidateStatus::Queued || r.status == CandidateStatus::Running ||
                         r.status == CandidateStatus::Deferred
                         ? std::string{} : r.storage)
            << ";" << quote(r.storageFallback) << ";"
            << r.ramBufferBytes << ";" << r.temporaryFileBytes << "\n";
    }
}

void LeaderboardWidget::exportToJson(const std::vector<CandidateResult> &results,
                                     const std::filesystem::path &path) {
    std::ofstream ofs(path);
    if (!ofs)
        return;
    Json output = Json::array();
    for (const auto &result : results)
        output.push_back(resultJson(result));
    ofs << output.dump(2) << '\n';
}

void LeaderboardWidget::render(const std::vector<CandidateResult> &results,
                               const CandidateResult &bestLeader, uint64_t originalBytes) {

    // 1. Top Leaderboard Banner / Metric Cards
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::CardBg);
    if (ImGui::BeginChild("LeaderboardHeaderCard", ImVec2(0, 75 * Theme::getDpiScale()), true,
                          ImGuiWindowFlags_NoScrollbar)) {
        if (ImGui::BeginTable("MetricsTable", 4)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            // Card 1: Original Size
            ImGui::TextColored(Theme::Colors::TextSecondary, "%s",
                               localized("UNCOMPRESSED CONTENT", "НЕСЖАТЫЕ ДАННЫЕ"));
            ImGui::TextColored(Theme::Colors::TextBright, "%s", formatBytes(originalBytes).c_str());
            ImGui::TableNextColumn();

            // Card 2: Current Best Leader
            ImGui::TextColored(Theme::Colors::TextSecondary, "%s",
                               localized("WHOLE-INPUT LEADER", "ЛИДЕР ДЛЯ ВСЕГО ВХОДА"));
            if (bestLeader.taskId > 0 && bestLeader.outputBytes > 0) {
                ImGui::TextColored(Theme::Colors::LeaderGreen, "%s",
                                   formatBytes(bestLeader.outputBytes).c_str());
            } else {
                ImGui::TextColored(Theme::Colors::TextSecondary, localized("None yet", "Пока нет"));
            }
            ImGui::TableNextColumn();

            // Card 3: Saved Bytes & Ratio
            ImGui::TextColored(Theme::Colors::TextSecondary, "%s",
                               localized("RATIO TO UNCOMPRESSED", "ДОЛЯ ОТ НЕСЖАТОГО"));
            if (bestLeader.taskId > 0 && bestLeader.outputBytes > 0 && originalBytes > 0) {
                double savedBytes = (originalBytes > bestLeader.outputBytes)
                                        ? (originalBytes - bestLeader.outputBytes)
                                        : 0;
                double savedPct = (savedBytes * 100.0) / static_cast<double>(originalBytes);
                std::stringstream ss;
                ss << std::fixed << std::setprecision(2) << bestLeader.compressionRatio << "% (-"
                   << savedPct << "%)";
                ImGui::TextColored(Theme::Colors::LeaderGreen, "%s", ss.str().c_str());
            } else {
                ImGui::TextColored(Theme::Colors::TextSecondary, "--");
            }
            ImGui::TableNextColumn();

            // Card 4: Tests Processed
            ImGui::TextColored(Theme::Colors::TextSecondary, "%s",
                               localized("TEST PROGRESS", "ХОД ПРОВЕРОК"));
            size_t completed = 0;
            for (const auto &r : results) {
                if (r.status != CandidateStatus::Queued && r.status != CandidateStatus::Running &&
                    r.status != CandidateStatus::Deferred) {
                    completed++;
                }
            }
            ImGui::TextColored(Theme::Colors::TextBright,
                               localized("%zu / %zu Candidates", "%zu / %zu кандидатов"), completed,
                               results.size());

            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // Filter bar
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(Theme::Colors::TextSecondary, "%s", L::tr(StringId::FilterPlaceholder));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260 * Theme::getDpiScale());
    ImGui::InputText("##CandidateFilter", m_filterBuffer, sizeof(m_filterBuffer));

    ImGui::SameLine();
    ImGui::TextColored(Theme::Colors::TextSecondary, "%s", L::tr(StringId::SortBy));

    ImGui::SameLine(ImGui::GetWindowWidth() - 250 * Theme::getDpiScale());
    if (ImGui::Button(L::tr(StringId::ExportCsv))) {
        exportToCsv(results, "results.csv");
    }
    ImGui::SameLine();
    if (ImGui::Button(L::tr(StringId::ExportJson))) {
        exportToJson(results, "results.json");
    }

    ImGui::Spacing();

    // 2. Interactive Candidates Table
    ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                 ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable |
                                 ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;

    if (ImGui::BeginTable("CandidatesTable", 9, tableFlags, ImVec2(0, 0))) {
        ImGui::TableSetupColumn(L::tr(StringId::ColId),
                                ImGuiTableColumnFlags_DefaultSort |
                                    ImGuiTableColumnFlags_WidthFixed,
                                40.0f * Theme::getDpiScale(), 0);
        ImGui::TableSetupColumn(L::tr(StringId::ColPreset), ImGuiTableColumnFlags_WidthFixed,
                                220.0f * Theme::getDpiScale(), 1);
        ImGui::TableSetupColumn(L::tr(StringId::ColFlags), ImGuiTableColumnFlags_WidthFixed,
                                260.0f * Theme::getDpiScale(), 2);
        ImGui::TableSetupColumn(L::tr(StringId::ColSize), ImGuiTableColumnFlags_WidthFixed,
                                105.0f * Theme::getDpiScale(), 3);
        ImGui::TableSetupColumn(L::tr(StringId::ColRatio), ImGuiTableColumnFlags_WidthFixed,
                                75.0f * Theme::getDpiScale(), 4);
        ImGui::TableSetupColumn(L::tr(StringId::ColTime), ImGuiTableColumnFlags_WidthFixed,
                                65.0f * Theme::getDpiScale(), 5);
        ImGui::TableSetupColumn(L::tr(StringId::ColRam), ImGuiTableColumnFlags_WidthFixed,
                                75.0f * Theme::getDpiScale(), 6);
        ImGui::TableSetupColumn(L::tr(StringId::ColStatus), ImGuiTableColumnFlags_WidthStretch,
                                0.0f, 7);
        ImGui::TableSetupColumn(localized("Storage", "Хранение"), ImGuiTableColumnFlags_WidthFixed,
                                105.0f * Theme::getDpiScale(), 8);
        ImGui::TableHeadersRow();

        auto localResults = results;

        // Sorting
        if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
            if (sorts_specs->SpecsDirty) {
                if (sorts_specs->SpecsCount > 0 && sorts_specs->Specs != nullptr) {
                    m_sortColumn = sorts_specs->Specs[0].ColumnIndex;
                    m_sortAscending =
                        (sorts_specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
                }
                sorts_specs->SpecsDirty = false;
            }
            if (localResults.size() > 1) {
                std::sort(
                    localResults.begin(), localResults.end(),
                    [&](const CandidateResult &a, const CandidateResult &b) {
                        bool asc = m_sortAscending;
                        switch (m_sortColumn) {
                        case 0:
                            return asc ? a.taskId < b.taskId : a.taskId > b.taskId;
                        case 1:
                            return asc ? a.name < b.name : a.name > b.name;
                        case 2:
                            return asc ? a.flagsStr < b.flagsStr : a.flagsStr > b.flagsStr;
                        case 3:
                            return asc ? a.outputBytes < b.outputBytes
                                       : a.outputBytes > b.outputBytes;
                        case 4:
                            return asc ? a.compressionRatio < b.compressionRatio
                                       : a.compressionRatio > b.compressionRatio;
                        case 5:
                            return asc ? a.durationSeconds < b.durationSeconds
                                       : a.durationSeconds > b.durationSeconds;
                        case 6:
                            return asc ? a.peakRamMB < b.peakRamMB : a.peakRamMB > b.peakRamMB;
                        case 7:
                            return asc ? static_cast<int>(a.status) < static_cast<int>(b.status)
                                       : static_cast<int>(a.status) > static_cast<int>(b.status);
                        case 8:
                            return asc ? a.storage < b.storage : a.storage > b.storage;
                        }
                        return false;
                    });
            }
        }

        std::string filterStr = m_filterBuffer;
        std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        // Compute stats for gradient
        std::vector<uint64_t> validSizes;
        for (const auto &c : localResults) {
            if (c.outputBytes > 0 &&
                (c.status == CandidateStatus::Passed || c.status == CandidateStatus::Verified)) {
                validSizes.push_back(c.outputBytes);
            }
        }
        std::sort(validSizes.begin(), validSizes.end());
        uint64_t q1 = validSizes.empty() ? 0 : validSizes[validSizes.size() / 4];
        uint64_t q3 = validSizes.empty() ? 0 : validSizes[(validSizes.size() * 3) / 4];

        for (const auto &cand : localResults) {
            if (!filterStr.empty()) {
                std::string nameLower = cand.name;
                std::transform(
                    nameLower.begin(), nameLower.end(), nameLower.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                std::string flagsLower = cand.flagsStr;
                std::transform(
                    flagsLower.begin(), flagsLower.end(), flagsLower.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (nameLower.find(filterStr) == std::string::npos &&
                    flagsLower.find(filterStr) == std::string::npos) {
                    continue;
                }
            }

            ImGui::TableNextRow();

            // Gradient color logic
            ImVec4 rowColor = Theme::Colors::TextPrimary;
            if (cand.isLeader) {
                rowColor = Theme::Colors::LeaderGreen;
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImColor(Theme::Colors::LeaderBg));
            } else if (cand.status == CandidateStatus::Failed ||
                       cand.status == CandidateStatus::OOM_Terminated) {
                rowColor = Theme::Colors::ErrorRed;
            } else if (cand.outputBytes > 0) {
                if (cand.outputBytes <= q1) {
                    rowColor = ImVec4(0.5f, 0.9f, 0.5f, 1.0f); // Light green for top 25%
                } else if (cand.outputBytes > q3) {
                    rowColor = Theme::Colors::WarningOrange; // Orange for bottom 25%
                }
            }

            // Col 0: ID
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(rowColor, "#%d", cand.taskId);

            // Col 1: Preset name
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(rowColor, "%s", cand.name.c_str());

            // Col 2: Flags Matrix
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(Theme::Colors::TextSecondary, "%s", cand.flagsStr.c_str());

            // Col 3: Output Size
            ImGui::TableSetColumnIndex(3);
            if (cand.outputBytes > 0) {
                std::string sz = formatBytes(cand.outputBytes);
                ImGui::TextColored(rowColor, "%s", sz.c_str());
            } else {
                ImGui::TextColored(Theme::Colors::TextSecondary, "--");
            }

            // Col 4: Ratio
            ImGui::TableSetColumnIndex(4);
            if (cand.outputBytes > 0 && originalBytes > 0) {
                std::stringstream rss;
                rss << std::fixed << std::setprecision(2) << cand.compressionRatio << "%";
                ImGui::TextColored(rowColor, "%s", rss.str().c_str());
            } else {
                ImGui::TextColored(Theme::Colors::TextSecondary, "--");
            }

            // Col 5: Duration
            ImGui::TableSetColumnIndex(5);
            if (cand.durationSeconds > 0.0) {
                std::stringstream tss;
                tss << std::fixed << std::setprecision(1) << cand.durationSeconds << "s";
                ImGui::TextColored(rowColor, "%s", tss.str().c_str());
            } else {
                ImGui::TextColored(Theme::Colors::TextSecondary, "--");
            }

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(localized("Compression: %.1f s\nVerification: %.1f s\nAdmission wait: %.1f s\nLeader gain: %llu bytes\nSample: %llu / %llu bytes\nAdmission: %s",
                                            "Сжатие: %.1f сек.\nПроверка: %.1f сек.\nОжидание допуска: %.1f сек.\nВыигрыш лидера: %llu байт\nСэмпл: %llu / %llu байт\nДопуск: %s"),
                                  cand.durationSeconds, cand.verificationSeconds, cand.waitSeconds,
                                  static_cast<unsigned long long>(cand.leaderGainBytes),
                                  static_cast<unsigned long long>(cand.sampleCoveredBytes),
                                  static_cast<unsigned long long>(cand.sampleTotalBytes), cand.admissionOrigin.c_str());

            // Col 6: RAM
            ImGui::TableSetColumnIndex(6);
            if (cand.peakRamMB > 0) {
                ImGui::TextColored(rowColor, "%llu MiB", static_cast<unsigned long long>(cand.peakRamMB));
            } else {
                ImGui::TextColored(Theme::Colors::TextSecondary, "--");
            }

            // Col 7: Status
            ImGui::TableSetColumnIndex(7);
            if (cand.isLeader) {
                ImGui::TextColored(Theme::Colors::LeaderGreen,
                                   localized("[BEST LEADER]", "[ЛУЧШИЙ КАНДИДАТ]"));
            } else {
                switch (cand.status) {
                case CandidateStatus::Queued:
                    ImGui::TextColored(Theme::Colors::TextSecondary,
                                       localized("Queued", "В очереди"));
                    break;
                case CandidateStatus::Deferred:
                    ImGui::TextColored(Theme::Colors::WarningOrange,
                                       localized("Deferred (admission limit)", "Отложен (лимит запуска)"));
                    break;
                case CandidateStatus::Running:
                    ImGui::TextColored(Theme::Colors::Accent,
                                       localized(">> Testing...", ">> Проверяется..."));
                    break;
                case CandidateStatus::Verified:
                    ImGui::TextColored(Theme::Colors::TextPrimary,
                                       cand.contentVerified
                                           ? localized("Manifest/SHA verified", "Проверен manifest/SHA")
                                           : localized("CRC tested; content pending", "CRC проверен; SHA ожидается"));
                    break;
                case CandidateStatus::Passed:
                    ImGui::TextColored(Theme::Colors::TextPrimary, localized("Passed", "Готов"));
                    break;
                case CandidateStatus::Skipped:
                    ImGui::TextColored(Theme::Colors::WarningOrange,
                                       localized("Skipped", "Пропущен"));
                    break;
                case CandidateStatus::DiscardedBySize:
                    ImGui::TextColored(Theme::Colors::TextSecondary,
                                       localized("Larger; verification omitted", "Крупнее; проверка не выполнялась"));
                    break;
                case CandidateStatus::OOM_Terminated:
                    ImGui::TextColored(Theme::Colors::ErrorRed,
                                       localized("Memory failure", "Ошибка памяти"));
                    break;
                case CandidateStatus::Failed:
                    ImGui::TextColored(Theme::Colors::ErrorRed, localized("Failed", "Ошибка"));
                    break;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(localized("Retries: %u | Explicit RAM allowance: %s | Tool exit: %d | Windows error: %u",
                                      "Повторов: %u | Явная надбавка RAM: %s | Код выхода: %d | Ошибка Windows: %u"),
                            cand.retryCount, cand.memoryOverride ? localized("yes", "да") : localized("no", "нет"),
                            cand.toolExitCode, cand.win32Error);
                ImGui::TextWrapped("%s", cand.statusMessage.c_str());
                if (cand.tempArchivePath.empty() && cand.verified)
                    ImGui::TextUnformatted(localized("Candidate artifact is no longer retained.", "Архив кандидата больше не хранится."));
                ImGui::EndTooltip();
            }

            ImGui::PushID(cand.taskId);
            if (ImGui::BeginPopupContextItem("candidate_ctx")) {
                if (ImGui::MenuItem(L::currentLang() ? "Выбрать и собрать" : "Select and assemble",
                                    nullptr, false,
                                    commandsEnabled && cand.verified &&
                                        !cand.tempArchivePath.empty() &&
                                        static_cast<bool>(selectCandidate)))
                    selectCandidate(cand.configId);
                if (ImGui::MenuItem(L::currentLang() ? "Повторить попытку" : "Retest", nullptr,
                                    false,
                                    retestEnabled &&
                                        (cand.status == CandidateStatus::Failed ||
                                         cand.status == CandidateStatus::Skipped ||
                                         cand.status == CandidateStatus::OOM_Terminated) &&
                                        static_cast<bool>(retestCandidate)))
                    retestCandidate(cand.configId);
                if (ImGui::MenuItem(L::tr(StringId::CopyFlags))) {
                    ImGui::SetClipboardText(cand.flagsStr.c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(8);
            const bool finished = cand.status != CandidateStatus::Queued && cand.status != CandidateStatus::Running &&
                                  cand.status != CandidateStatus::Deferred;
            if (!finished)
                ImGui::TextDisabled("--");
            else if (cand.storage == "ram")
                ImGui::TextUnformatted(cand.temporaryFileBytes
                    ? localized("RAM + disk", "RAM + диск") : "RAM");
            else
                ImGui::TextUnformatted(localized("Disk", "Диск"));
            if (finished && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
                ImGui::Text(localized("Trial archive: %s", "Пробный архив: %s"),
                    cand.storage == "ram" ? "RAM" : localized("Disk", "Диск"));
                ImGui::Text(localized("RAM output buffer: %s", "Выходной буфер RAM: %s"),
                    (cand.ramBufferBytes ? formatBytes(cand.ramBufferBytes) : "0 B").c_str());
                ImGui::Text(localized("Logical candidate file bytes: %s", "Логический объём файла кандидата: %s"),
                    (cand.temporaryFileBytes ? formatBytes(cand.temporaryFileBytes) : "0 B").c_str());
                if (!cand.storageFallback.empty())
                    ImGui::TextWrapped(localized("Disk fallback: %s", "Причина перехода на диск: %s"),
                                       cand.storageFallback.c_str());
                ImGui::TextWrapped("%s", localized(
                    "File bytes describe an archive created during this attempt, even if later removed. They are not process I/O or physical SSD writes.",
                    "Объём относится к созданному архиву этой попытки, даже если он позднее удалён. Это не I/O процесса и не физическая запись SSD."));
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }

        ImGui::EndTable();
    }
}

} // namespace UltraArch
