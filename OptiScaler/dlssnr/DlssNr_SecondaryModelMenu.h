#pragma once

#include "worker/WorkerModelCatalog.h"
#include <Config.h>
#include <imgui/imgui.h>

namespace DlssNr::Worker
{
// A runtime choice for the whole contiguous secondary pass group. Discovery
// runs on first use and explicit refresh, never on every rendered frame.
class SecondaryModelMenu
{
  public:
    static void Render(Config& config, const std::vector<std::filesystem::path>& roots)
    {
        static auto models = ModelCatalog::Discover(roots);
        auto choice = config.DlssNrSecondaryModel.value_or_default();
        const bool inherited = choice.empty() || choice == "auto";
        if (ImGui::BeginCombo("Second GPU runtime", inherited ? "Same as game GPU" : choice.c_str()))
        {
            if (ImGui::Selectable("Same as game GPU", inherited))
                config.DlssNrSecondaryModel = std::string();
            for (const auto& model : models)
                if (ImGui::Selectable(model.c_str(), choice == model))
                    config.DlssNrSecondaryModel = model;
            ImGui::EndCombo();
        }
        if (ImGui::SmallButton("Refresh runtime list"))
            models = ModelCatalog::Discover(roots);
        ImGui::TextWrapped("Add compatible runtimes in OptiScaler/NR-Models/<name>/nvngx_dlssnr.dll. "
                           "All passes on the second GPU use this runtime; their presets remain per-pass.");
    }
};
} // namespace DlssNr::Worker
