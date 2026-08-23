#pragma once

// AF-side implementation of the shipped C++ integration API (api/AddictionFramework_API.h). The concrete
// interface object is a file-local singleton in PluginAPI.cpp that delegates 1:1 to AddictionManager —
// the exact same core the Papyrus `Addiction.*` natives call. Delivery: a GetProcAddress export
// (AF_RequestPluginAPI, defined in PluginAPI.cpp) plus an SKSE-messaging broadcast wired from main.cpp.
namespace AddictionFramework::PluginAPI
{
    // Broadcast the interface to every SKSE listener. Call once at AF's kPostLoad — consumers that
    // registered an "AddictionFramework"-filtered listener in their SKSEPlugin_Load receive it, whether
    // they loaded before or after AF (SKSE runs every plugin's Load before dispatching any kPostLoad).
    void BroadcastInterface();
}
