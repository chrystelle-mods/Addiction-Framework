#include "Papyrus.h"

#include "Addiction.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace
{
    // ---- Real public API — class "Addiction" (§9). Categories are addressed by STRING (the config key);
    // the group Form stays internal. An unknown/typo'd name resolves to key 0, which every manager call
    // treats as a safe no-op (level 0 / not addicted / ignored). ----
    std::uint32_t KeyOf(RE::BSFixedString a_name)
    {
        return AddictionFramework::AddictionManager::GetSingleton()->KeyForName(a_name.c_str());
    }

    float API_GetLevel(RE::StaticFunctionTag*, RE::BSFixedString asCategory)
    {
        return AddictionFramework::AddictionManager::GetSingleton()->GetLevel(KeyOf(asCategory));
    }
    std::int32_t API_GetStage(RE::StaticFunctionTag*, RE::BSFixedString asCategory)
    {
        return static_cast<std::int32_t>(
            AddictionFramework::AddictionManager::GetSingleton()->GetStage(KeyOf(asCategory)));
    }
    bool API_IsAddicted(RE::StaticFunctionTag*, RE::BSFixedString asCategory)
    {
        return AddictionFramework::AddictionManager::GetSingleton()->IsAddicted(KeyOf(asCategory));
    }
    bool API_IsInAcuteStatus(RE::StaticFunctionTag*, RE::BSFixedString asCategory)
    {
        return AddictionFramework::AddictionManager::GetSingleton()->IsInAcuteStatus(KeyOf(asCategory));
    }
    bool API_IsIntoxicated(RE::StaticFunctionTag*, RE::BSFixedString asCategory)
    {
        return AddictionFramework::AddictionManager::GetSingleton()->IsIntoxicated(KeyOf(asCategory));
    }
    float API_GetAcuteLevel(RE::StaticFunctionTag*, RE::BSFixedString asCategory)
    {
        return AddictionFramework::AddictionManager::GetSingleton()->GetAcuteLevel(KeyOf(asCategory));
    }
    float API_GetAcutePercent(RE::StaticFunctionTag*, RE::BSFixedString asCategory)
    {
        return AddictionFramework::AddictionManager::GetSingleton()->GetAcutePercent(KeyOf(asCategory));
    }
    bool API_IsAcuteEffectActive(RE::StaticFunctionTag*, RE::BSFixedString asEffect)
    {
        // asEffect is a shared acute-status NAME (Drunk/High/Stoned/Wired), not a category.
        const char* key = asEffect.c_str();
        return AddictionFramework::AddictionManager::GetSingleton()->IsAcuteEffectActive(key ? key : "");
    }
    float API_NotifyUse(RE::StaticFunctionTag*, RE::BSFixedString asCategory, float amount)
    {
        return AddictionFramework::AddictionManager::GetSingleton()->NotifyUse(KeyOf(asCategory), amount);
    }
    void API_AddLevel(RE::StaticFunctionTag*, RE::BSFixedString asCategory, float amount)
    {
        AddictionFramework::AddictionManager::GetSingleton()->AddLevel(KeyOf(asCategory), amount);
    }
    void API_Cure(RE::StaticFunctionTag*, RE::BSFixedString asCategory)
    {
        AddictionFramework::AddictionManager::GetSingleton()->Cure(KeyOf(asCategory));
    }
    void API_CureAll(RE::StaticFunctionTag*)
    {
        AddictionFramework::AddictionManager::GetSingleton()->CureAll();
    }
    void API_ResetWithdrawalTimer(RE::StaticFunctionTag*, RE::BSFixedString asCategory)
    {
        AddictionFramework::AddictionManager::GetSingleton()->ResetWithdrawalTimer(KeyOf(asCategory));
    }
    std::vector<RE::BSFixedString> API_GetActiveAddictions(RE::StaticFunctionTag*)
    {
        std::vector<RE::BSFixedString> out;
        auto*                          mgr = AddictionFramework::AddictionManager::GetSingleton();
        for (auto key : mgr->GetActiveAddictions()) {
            const std::string nm = mgr->NameForKey(key);
            if (!nm.empty()) {
                out.emplace_back(nm);
            }
        }
        return out;
    }

    // Parse the modifier-kind string (case-insensitive) → ModKind. A typo returns nullopt (logged no-op).
    std::optional<AddictionFramework::ModKind> ParseKind(RE::BSFixedString a_kind)
    {
        std::string k = a_kind.c_str() ? a_kind.c_str() : "";
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (k == "gain") {
            return AddictionFramework::ModKind::kGain;
        }
        if (k == "decay") {
            return AddictionFramework::ModKind::kDecay;
        }
        if (k == "withdrawal") {
            return AddictionFramework::ModKind::kWithdrawal;
        }
        return std::nullopt;
    }

    void API_AddModifier(RE::StaticFunctionTag*, RE::BSFixedString asCategory, RE::BSFixedString asKind, float afPercent)
    {
        const auto kind = ParseKind(asKind);
        if (!kind) {
            logger::warn("Addiction.AddModifier: bad kind '{}' (want gain|decay|withdrawal) — ignored.",
                         asKind.c_str());
            return;
        }
        AddictionFramework::AddictionManager::GetSingleton()->AddModifier(KeyOf(asCategory), *kind, afPercent);
    }
    void API_ClearModifier(RE::StaticFunctionTag*, RE::BSFixedString asCategory, RE::BSFixedString asKind)
    {
        const auto kind = ParseKind(asKind);
        if (!kind) {
            logger::warn("Addiction.ClearModifier: bad kind '{}' (want gain|decay|withdrawal) — ignored.",
                         asKind.c_str());
            return;
        }
        AddictionFramework::AddictionManager::GetSingleton()->ClearModifier(KeyOf(asCategory), *kind);
    }
    float API_GetModifier(RE::StaticFunctionTag*, RE::BSFixedString asCategory, RE::BSFixedString asKind)
    {
        const auto kind = ParseKind(asKind);
        if (!kind) {
            logger::warn("Addiction.GetModifier: bad kind '{}' (want gain|decay|withdrawal) — returning 0.",
                         asKind.c_str());
            return 0.0f;
        }
        return AddictionFramework::AddictionManager::GetSingleton()->GetModifier(KeyOf(asCategory), *kind);
    }
}

bool AddictionFramework::Papyrus::Bind(RE::BSScript::IVirtualMachine* a_vm)
{
    // Public API (class "Addiction", §9) — string-keyed, master-free.
    a_vm->RegisterFunction("GetLevel", "Addiction", API_GetLevel);
    a_vm->RegisterFunction("GetStage", "Addiction", API_GetStage);
    a_vm->RegisterFunction("IsAddicted", "Addiction", API_IsAddicted);
    a_vm->RegisterFunction("IsInAcuteStatus", "Addiction", API_IsInAcuteStatus);
    a_vm->RegisterFunction("IsIntoxicated", "Addiction", API_IsIntoxicated);
    a_vm->RegisterFunction("GetAcuteLevel", "Addiction", API_GetAcuteLevel);
    a_vm->RegisterFunction("GetAcutePercent", "Addiction", API_GetAcutePercent);
    a_vm->RegisterFunction("IsAcuteEffectActive", "Addiction", API_IsAcuteEffectActive);
    a_vm->RegisterFunction("NotifyUse", "Addiction", API_NotifyUse);
    a_vm->RegisterFunction("AddLevel", "Addiction", API_AddLevel);
    a_vm->RegisterFunction("Cure", "Addiction", API_Cure);
    a_vm->RegisterFunction("CureAll", "Addiction", API_CureAll);
    a_vm->RegisterFunction("ResetWithdrawalTimer", "Addiction", API_ResetWithdrawalTimer);
    a_vm->RegisterFunction("GetActiveAddictions", "Addiction", API_GetActiveAddictions);
    a_vm->RegisterFunction("AddModifier", "Addiction", API_AddModifier);
    a_vm->RegisterFunction("ClearModifier", "Addiction", API_ClearModifier);
    a_vm->RegisterFunction("GetModifier", "Addiction", API_GetModifier);

    logger::info("AddictionFramework: registered public API (Addiction.*).");
    return true;
}
