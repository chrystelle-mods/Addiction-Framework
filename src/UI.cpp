#include "UI.h"

#include <filesystem>  // SKSEMenuFramework.h's IsInstalled() uses std::filesystem
#include <string>

#include "Addiction.h"  // AddictionManager::CureAll (maintenance page)
#include "Blackout.h"
#include "SKSEMenuFramework.h"  // vendored: extern/SKSEMenuFramework (header-only, soft-links the DLL)

// This framework fork renames the ImGui namespace to ImGuiMCP (so it can't clash with other ImGui-using
// plugins). Alias it back so standard ImGui snippets read normally in this translation unit.
namespace ImGui = ImGuiMCP;

namespace
{
    // Runs on the framework's ImGui render thread. Edits the per-scenario chances in place (in the
    // BlackoutManager); the selector reads them on the main thread when a blackout fires. Only benign
    // float races (same model as OSafeThread's settings page) — the map's structure is fixed after load.
    void __stdcall RenderBlackoutScenarios()
    {
        auto*       mgr  = AddictionFramework::BlackoutManager::GetSingleton();
        const auto& outs = mgr->Outcomes();

        ImGui::TextWrapped(
            "When the player blacks out, one outcome is chosen at random by weight. Set each scenario's "
            "chance below (0-100).");
        ImGui::Spacing();
        ImGui::BulletText("If the total is UNDER 100, the rest is the default.");
        ImGui::BulletText("If the total is OVER 100, the chances are scaled down to proportions "
                          "(e.g. two at 100 = 50%% each).");
        ImGui::BulletText("Default = fade to black, then wake ~6 game-hours later. No scenario needed.");
        ImGui::Separator();
        ImGui::Spacing();

        if (outs.empty()) {
            ImGui::TextDisabled("No blackout scenarios are installed.");
            ImGui::TextWrapped("Every blackout uses the built-in default (fade + ~6h). Install a blackout "
                               "scenario mod to add outcomes here.");
            return;
        }

        // Total of the current chances → the normalization denominator + the default remainder.
        float total = 0.0f;
        for (const auto& o : outs) {
            total += mgr->GetChance(o.name);
        }
        const float denom      = (total <= 100.0f) ? 100.0f : total;
        const float defaultPct = (total <= 100.0f) ? (100.0f - total) : 0.0f;

        int i = 0;
        for (const auto& o : outs) {
            const std::string label = o.name + "##af_bo_" + std::to_string(i++);
            ImGui::SliderFloat(label.c_str(), mgr->ChancePtr(o.name), 0.0f, 100.0f, "%.0f");

            const float chance = mgr->GetChance(o.name);
            const float eff    = (denom > 0.0f) ? (chance / denom * 100.0f) : 0.0f;
            ImGui::SameLine();
            ImGui::TextDisabled(" -> %.0f%%", eff);

            if (!o.description.empty()) {
                ImGui::TextWrapped("%s", o.description.c_str());
            }
            ImGui::Spacing();
        }

        ImGui::Separator();
        ImGui::Text("Default (fade + ~6h): %.0f%%", defaultPct);
        ImGui::Spacing();
        ImGui::TextDisabled("Changes are saved with your game.");
    }

    // Maintenance / panic cleanup. Runs on the framework's ImGui render thread, so the actual game-state
    // change is queued onto the MAIN thread via the SKSE task interface.
    void __stdcall RenderMaintenance()
    {
        ImGui::TextWrapped("If a buggy or auto-consume consumable (from another mod) left you with a stuck "
                           "Addiction Framework effect, this clears it.");
        ImGui::Spacing();
        if (ImGui::Button("Remove all Addiction Framework effects")) {
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([] { AddictionFramework::AddictionManager::GetSingleton()->CureAll(); });
            }
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Cures every category and strips all AF-applied spells/effects off you.");
    }
}

void AddictionFramework::UI::Register()
{
    if (!SKSEMenuFramework::IsInstalled()) {
        logger::info("UI: SKSE Menu Framework not installed — Addiction Framework pages not registered.");
        return;
    }
    SKSEMenuFramework::SetSection("Addiction Framework");
    SKSEMenuFramework::AddSectionItem("Blackout Scenarios", RenderBlackoutScenarios);
    SKSEMenuFramework::AddSectionItem("Maintenance", RenderMaintenance);
    logger::info("UI: registered the Addiction Framework menu pages (SKSE Menu Framework).");
}
