#include "PCH.h"

#include "Addiction.h"
#include "Blackout.h"
#include "Casting.h"
#include "Config.h"
#include "Hooks.h"
#include "Papyrus.h"
#include "PluginAPI.h"
#include "UI.h"

using namespace SKSE;
using namespace SKSE::log;
using namespace SKSE::stl;

namespace {
    void InitializeLogging() {
        auto path = log_directory();
        if (!path) {
            report_and_fail("AddictionFramework: unable to lookup SKSE logs directory."sv);
        }
        *path /= PluginDeclaration::GetSingleton()->GetName();  // AddictionFramework.log
        *path += L".log";

        std::shared_ptr<spdlog::logger> log;
        if (IsDebuggerPresent()) {
            log = std::make_shared<spdlog::logger>("Global", std::make_shared<spdlog::sinks::msvc_sink_mt>());
        } else {
            log = std::make_shared<spdlog::logger>(
                "Global", std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true));
        }
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);

        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%s:%#] %v");
    }

    // --- SKSE co-save callbacks (forward to the AddictionManager) ---
    void SaveCallback(SKSE::SerializationInterface* a_intfc) {
        AddictionFramework::AddictionManager::GetSingleton()->Save(a_intfc);
    }
    void LoadCallback(SKSE::SerializationInterface* a_intfc) {
        AddictionFramework::AddictionManager::GetSingleton()->Load(a_intfc);
    }
    void RevertCallback(SKSE::SerializationInterface* a_intfc) {
        AddictionFramework::AddictionManager::GetSingleton()->Revert();
    }

    void MessageHandler(SKSE::MessagingInterface::Message* a_msg) {
        switch (a_msg->type) {
            case SKSE::MessagingInterface::kPostLoad:
                // Pure engine-code vtable hook — safe to install here.
                AddictionFramework::Hooks::Install();
                // Push the C++ integration interface to any listener (order-independent: consumers
                // register their "AddictionFramework"-filtered listener in their own SKSEPlugin_Load).
                AddictionFramework::PluginAPI::BroadcastInterface();
                break;
            case SKSE::MessagingInterface::kDataLoaded:
                AddictionFramework::Config::Load();          // parse JSON, register categories, stamp ENIT
                AddictionFramework::BlackoutManager::GetSingleton()->Init();  // fade imods + scenario registry
                AddictionFramework::UI::Register();           // SKSE Menu Framework page (no-op if absent)
                AddictionFramework::CastingSpike::Install();  // recon: log player casts + school
                logger::info("kDataLoaded reached — AddictionFramework is live.");
                break;
            default:
                break;
        }
    }
}  // namespace

extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() {
    SKSE::PluginVersionData v;
    v.PluginVersion(REL::Version("0.1.0.0"sv));
    v.PluginName("AddictionFramework");
    v.AuthorName("Lacey");
    v.UsesAddressLibrary();  // one DLL across SE / AE / VR
    v.UsesNoStructs();
    return v;
}();

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* a_info) {
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = "AddictionFramework";
    a_info->version = 0x00010000;  // 0.1.0.0 packed
    return true;
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const LoadInterface* a_skse) {
    InitializeLogging();

    auto* plugin = PluginDeclaration::GetSingleton();
    logger::info("{} {} is loading...", plugin->GetName(), plugin->GetVersion());

    Init(a_skse);

    if (!SKSE::GetMessagingInterface()->RegisterListener(MessageHandler)) {
        return false;
    }

    if (auto* papyrus = SKSE::GetPapyrusInterface()) {
        papyrus->Register(AddictionFramework::Papyrus::Bind);
    }

    if (auto* serial = SKSE::GetSerializationInterface()) {
        serial->SetUniqueID(AddictionFramework::AddictionManager::kSerializationID);
        serial->SetSaveCallback(SaveCallback);
        serial->SetLoadCallback(LoadCallback);
        serial->SetRevertCallback(RevertCallback);
        logger::info("AddictionFramework: registered co-save serialization callbacks.");
    }

    logger::info("{} finished loading.", plugin->GetName());
    return true;
}
