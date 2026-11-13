#include "pch.h"
#include "NotificationsPage.h"

#include "menu_common.h"
#include <imgui/ImGuiNotify.hpp>
#include <misc/NotificationHistory.h>

#include <ctime>

namespace
{
const ImVec4 kGreen(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kAmber(1.0f, 0.85f, 0.3f, 1.0f);
const ImVec4 kRed(1.0f, 0.4f, 0.35f, 1.0f);
const ImVec4 kBlue(0.45f, 0.7f, 1.0f, 1.0f);

ImVec4 ColorOf(uint8_t type)
{
    switch (static_cast<ImGuiToastType>(type))
    {
    case ImGuiToastType::Success:
        return kGreen;
    case ImGuiToastType::Warning:
        return kAmber;
    case ImGuiToastType::Error:
        return kRed;
    case ImGuiToastType::Info:
        return kBlue;
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

const char* LabelOf(uint8_t type)
{
    switch (static_cast<ImGuiToastType>(type))
    {
    case ImGuiToastType::Success:
        return "Success";
    case ImGuiToastType::Warning:
        return "Warning";
    case ImGuiToastType::Error:
        return "Error";
    case ImGuiToastType::Info:
        return "Info";
    default:
        return "Note";
    }
}

std::string ClockOf(std::chrono::system_clock::time_point time)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm local {};

    if (localtime_s(&local, &t) != 0)
        return "--:--:--";

    char text[16] {};
    std::strftime(text, sizeof(text), "%H:%M:%S", &local);
    return text;
}
} // namespace

void NotificationsPage::Render()
{
    const auto entries = NotificationHistory::Snapshot(); // a copy: toasts keep arriving from other threads

    ImGui::TextWrapped("Every notification shown since the game started, newest first. The overlay's toasts fade after "
                       "a few seconds; the ones that matter stay here.");
    ImGui::TextDisabled("%zu listed, %llu shown in total", entries.size(),
                        (unsigned long long) NotificationHistory::Total());

    if (ImGui::Button("Clear"))
        NotificationHistory::Clear();

    ImGui::Spacing();

    if (entries.empty())
    {
        ImGui::TextDisabled("Nothing yet.");
        return;
    }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;

    if (!ImGui::BeginTable("notification_history", 3, flags))
        return;

    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
    {
        const auto& entry = *it;
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        const auto first = ClockOf(entry.first);

        if (entry.count > 1)
        {
            // A repeat: first and last time, and how often
            ImGui::TextUnformatted(first.c_str());
            ImGui::TextDisabled("to %s", ClockOf(entry.last).c_str());
        }
        else
            ImGui::TextUnformatted(first.c_str());

        ImGui::TableNextColumn();
        ImGui::TextColored(ColorOf(entry.type), "%s", LabelOf(entry.type));

        ImGui::TableNextColumn();

        if (!entry.title.empty())
            ImGui::TextColored(ColorOf(entry.type), "%s%s", entry.title.c_str(),
                               entry.count > 1 ? std::format("  (x{})", entry.count).c_str() : "");

        if (!entry.content.empty())
            ImGui::TextWrapped("%s", entry.content.c_str());
    }

    ImGui::EndTable();
}
