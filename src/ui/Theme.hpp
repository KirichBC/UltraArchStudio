#pragma once

#include "imgui.h"

namespace UltraArch {
namespace Theme {
inline float g_dpiScale = 1.0f;

// Windows 11 Dark Fluent UI Color Palette
namespace Colors {
// Base backgrounds
inline const ImVec4 WindowBg = ImVec4(0.102f, 0.106f, 0.118f, 1.0f);    // #1a1b1e
inline const ImVec4 ChildBg = ImVec4(0.125f, 0.129f, 0.145f, 1.0f);     // #202125
inline const ImVec4 CardBg = ImVec4(0.145f, 0.149f, 0.169f, 1.0f);      // #25262b
inline const ImVec4 CardHovered = ImVec4(0.173f, 0.180f, 0.204f, 1.0f); // #2c2e34
inline const ImVec4 Border = ImVec4(0.173f, 0.180f, 0.200f, 0.8f);      // #2c2e33

// Accent (Windows 11 Blue)
inline const ImVec4 Accent = ImVec4(0.133f, 0.545f, 0.902f, 1.0f);        // #228be6
inline const ImVec4 AccentHovered = ImVec4(0.110f, 0.494f, 0.839f, 1.0f); // #1c7ed6
inline const ImVec4 AccentActive = ImVec4(0.094f, 0.443f, 0.761f, 1.0f);  // #1871c2

// Status colors
inline const ImVec4 LeaderGreen = ImVec4(0.251f, 0.753f, 0.341f, 1.0f); // #40c057
inline const ImVec4 LeaderBg = ImVec4(0.169f, 0.541f, 0.243f, 0.25f);
inline const ImVec4 WarningOrange = ImVec4(0.980f, 0.549f, 0.086f, 1.0f); // #fa8c16
inline const ImVec4 ErrorRed = ImVec4(0.980f, 0.329f, 0.329f, 1.0f);      // #fa5252

// Text
inline const ImVec4 TextPrimary = ImVec4(0.757f, 0.761f, 0.773f, 1.0f);   // #c1c2c5
inline const ImVec4 TextSecondary = ImVec4(0.565f, 0.573f, 0.588f, 1.0f); // #909296
inline const ImVec4 TextBright = ImVec4(0.960f, 0.965f, 0.976f, 1.0f);    // #f8f9fa
} // namespace Colors

inline void applyDarkFluentStyle() {
    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle(); // Rebuild from unscaled metrics; monitor changes never accumulate scale.

    // Windows 11 fluent rounding
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 6.0f;

    // Spacing and sizing
    style.WindowPadding = ImVec2(14.0f, 14.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
    style.CellPadding = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 12.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_Text] = Colors::TextPrimary;
    colors[ImGuiCol_TextDisabled] = Colors::TextSecondary;
    colors[ImGuiCol_WindowBg] = Colors::WindowBg;
    colors[ImGuiCol_ChildBg] = Colors::ChildBg;
    colors[ImGuiCol_PopupBg] = Colors::CardBg;
    colors[ImGuiCol_Border] = Colors::Border;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg] = Colors::CardBg;
    colors[ImGuiCol_FrameBgHovered] = Colors::CardHovered;
    colors[ImGuiCol_FrameBgActive] = Colors::AccentActive;
    colors[ImGuiCol_TitleBg] = Colors::ChildBg;
    colors[ImGuiCol_TitleBgActive] = Colors::ChildBg;
    colors[ImGuiCol_TitleBgCollapsed] = Colors::WindowBg;
    colors[ImGuiCol_MenuBarBg] = Colors::ChildBg;
    colors[ImGuiCol_ScrollbarBg] = Colors::WindowBg;
    colors[ImGuiCol_ScrollbarGrab] = Colors::CardHovered;
    colors[ImGuiCol_ScrollbarGrabHovered] = Colors::AccentHovered;
    colors[ImGuiCol_ScrollbarGrabActive] = Colors::AccentActive;
    colors[ImGuiCol_CheckMark] = Colors::Accent;
    colors[ImGuiCol_SliderGrab] = Colors::Accent;
    colors[ImGuiCol_SliderGrabActive] = Colors::AccentActive;
    colors[ImGuiCol_Button] = Colors::CardBg;
    colors[ImGuiCol_ButtonHovered] = Colors::CardHovered;
    colors[ImGuiCol_ButtonActive] = Colors::AccentActive;
    colors[ImGuiCol_Header] = Colors::CardBg;
    colors[ImGuiCol_HeaderHovered] = Colors::CardHovered;
    colors[ImGuiCol_HeaderActive] = Colors::AccentActive;
    colors[ImGuiCol_Separator] = Colors::Border;
    colors[ImGuiCol_SeparatorHovered] = Colors::AccentHovered;
    colors[ImGuiCol_SeparatorActive] = Colors::AccentActive;
    colors[ImGuiCol_ResizeGrip] = Colors::CardHovered;
    colors[ImGuiCol_ResizeGripHovered] = Colors::AccentHovered;
    colors[ImGuiCol_ResizeGripActive] = Colors::AccentActive;
    colors[ImGuiCol_Tab] = Colors::CardBg;
    colors[ImGuiCol_TabHovered] = Colors::CardHovered;
    colors[ImGuiCol_TabActive] = Colors::Accent;
    colors[ImGuiCol_TabUnfocused] = Colors::WindowBg;
    colors[ImGuiCol_TabUnfocusedActive] = Colors::CardBg;
    colors[ImGuiCol_TableHeaderBg] = Colors::ChildBg;
    colors[ImGuiCol_TableBorderStrong] = Colors::Border;
    colors[ImGuiCol_TableBorderLight] =
        ImVec4(Colors::Border.x, Colors::Border.y, Colors::Border.z, 0.4f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.015f);
    style.ScaleAllSizes(g_dpiScale);
    style.FontScaleDpi = g_dpiScale;
}

inline void applyLightFluentStyle() {
    ImGuiStyle &style = ImGui::GetStyle();
    ImGui::StyleColorsLight(&style);
    // Basic light styles
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 6.0f;

    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.95f, 0.95f, 0.96f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

inline float getDpiScale() {
    return g_dpiScale;
}
inline void applyDpiScale(float scale) {
    g_dpiScale = scale > 0 ? scale : 1.0f;
}

inline void loadFont(const char *fontPath, float sizePixels) {
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    io.Fonts->AddFontFromFileTTF(fontPath, sizePixels * g_dpiScale);
    if (!io.Fonts->Fonts.empty()) {
        io.Fonts->Build();
    } else {
        io.Fonts->AddFontDefault();
    }
}

} // namespace Theme
} // namespace UltraArch
