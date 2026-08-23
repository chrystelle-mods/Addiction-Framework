#include "PluginAPI.h"

#include "Addiction.h"
#include "AddictionFramework_API.h"  // the shipped interface definition (api/)

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace
{
    using namespace AddictionFramework;

    // Category name → internal group key. Null / unknown / typo'd → 0, which every manager call treats as
    // a safe no-op (level 0 / not addicted / ignored) — identical to the Papyrus API's behavior.
    std::uint32_t KeyOf(const char* a_category)
    {
        return AddictionManager::GetSingleton()->KeyForName(a_category ? a_category : "");
    }

    // "gain" | "decay" | "withdrawal" (case-insensitive) → ModKind; anything else → nullopt (logged no-op).
    std::optional<ModKind> ParseKind(const char* a_kind)
    {
        std::string k = a_kind ? a_kind : "";
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (k == "gain") {
            return ModKind::kGain;
        }
        if (k == "decay") {
            return ModKind::kDecay;
        }
        if (k == "withdrawal") {
            return ModKind::kWithdrawal;
        }
        return std::nullopt;
    }

    // The concrete interface. A THIN export layer: every method forwards to AddictionManager, the same
    // singleton src/Papyrus.cpp's API_* natives call — no logic of its own. Main-thread only (documented
    // in the shipped header): these touch the manager, player, and magic effects.
    class AddictionFrameworkAPIImpl final : public AddictionFrameworkAPI::IVAddictionFramework1
    {
    public:
        std::uint32_t GetVersion() override { return AddictionFrameworkAPI::kInterfaceVersion1; }

        float NotifyUse(const char* a_category, float a_amount) override
        {
            return AddictionManager::GetSingleton()->NotifyUse(KeyOf(a_category), a_amount);
        }
        float GetLevel(const char* a_category) override
        {
            return AddictionManager::GetSingleton()->GetLevel(KeyOf(a_category));
        }
        std::int32_t GetStage(const char* a_category) override
        {
            return static_cast<std::int32_t>(AddictionManager::GetSingleton()->GetStage(KeyOf(a_category)));
        }
        bool IsAddicted(const char* a_category) override
        {
            return AddictionManager::GetSingleton()->IsAddicted(KeyOf(a_category));
        }
        void AddLevel(const char* a_category, float a_amount) override
        {
            AddictionManager::GetSingleton()->AddLevel(KeyOf(a_category), a_amount);
        }
        void AddModifier(const char* a_category, const char* a_kind, float a_percent) override
        {
            if (const auto kind = ParseKind(a_kind)) {
                AddictionManager::GetSingleton()->AddModifier(KeyOf(a_category), *kind, a_percent);
            } else {
                logger::warn("PluginAPI::AddModifier: bad kind '{}' (want gain|decay|withdrawal) — ignored.",
                             a_kind ? a_kind : "(null)");
            }
        }
        void ClearModifier(const char* a_category, const char* a_kind) override
        {
            if (const auto kind = ParseKind(a_kind)) {
                AddictionManager::GetSingleton()->ClearModifier(KeyOf(a_category), *kind);
            } else {
                logger::warn("PluginAPI::ClearModifier: bad kind '{}' (want gain|decay|withdrawal) — ignored.",
                             a_kind ? a_kind : "(null)");
            }
        }
        float GetModifier(const char* a_category, const char* a_kind) override
        {
            if (const auto kind = ParseKind(a_kind)) {
                return AddictionManager::GetSingleton()->GetModifier(KeyOf(a_category), *kind);
            }
            logger::warn("PluginAPI::GetModifier: bad kind '{}' (want gain|decay|withdrawal) — returning 0.",
                         a_kind ? a_kind : "(null)");
            return 0.0f;
        }
        void Cure(const char* a_category) override
        {
            AddictionManager::GetSingleton()->Cure(KeyOf(a_category));
        }
        void ResetWithdrawalTimer(const char* a_category) override
        {
            AddictionManager::GetSingleton()->ResetWithdrawalTimer(KeyOf(a_category));
        }
        bool IsCategoryActive(const char* a_category) override
        {
            // Registered (activated by a config) ⟺ the name resolves to a non-zero group key. Inert roster
            // slots and unknown names never register a name → key 0.
            return AddictionManager::GetSingleton()->KeyForName(a_category ? a_category : "") != 0;
        }
        bool IsInAcuteStatus(const char* a_category) override
        {
            return AddictionManager::GetSingleton()->IsInAcuteStatus(KeyOf(a_category));
        }
        bool IsIntoxicated(const char* a_category) override
        {
            return AddictionManager::GetSingleton()->IsIntoxicated(KeyOf(a_category));
        }
        float GetAcuteLevel(const char* a_category) override
        {
            return AddictionManager::GetSingleton()->GetAcuteLevel(KeyOf(a_category));
        }
        void CureAll() override { AddictionManager::GetSingleton()->CureAll(); }
    };

    // Process-lifetime singletons: the interface object + the messaging payload that points at it.
    AddictionFrameworkAPIImpl               g_apiImpl;
    AddictionFrameworkAPI::InterfaceMessage g_apiMessage{ AddictionFrameworkAPI::kInterfaceVersion1,
                                                          &g_apiImpl };
}

// GetProcAddress target (mechanism A). extern "C" → exported undecorated as "AF_RequestPluginAPI" on x64.
// Returns the singleton, or nullptr if the caller wants a newer ABI than we implement.
extern "C" DLLEXPORT void* AF_RequestPluginAPI(std::uint32_t a_abiVersion)
{
    if (a_abiVersion > AddictionFrameworkAPI::kInterfaceVersion1) {
        return nullptr;
    }
    return &g_apiImpl;
}

namespace AddictionFramework::PluginAPI
{
    void BroadcastInterface()
    {
        if (auto* messaging = SKSE::GetMessagingInterface()) {
            messaging->Dispatch(AddictionFrameworkAPI::kMessage_DeliverInterface, &g_apiMessage,
                                sizeof(g_apiMessage), nullptr);  // nullptr receiver = all listeners
            logger::info("PluginAPI: broadcast C++ interface v{} to listeners.",
                         static_cast<int>(AddictionFrameworkAPI::kInterfaceVersion1));
        }
    }
}
