#pragma once

// AF — the in-game Blackout Scenarios page, built on the SKSE Menu Framework. Register() is a no-op when
// the framework isn't installed, so it's always safe to call (AF does not hard-depend on it).

namespace AddictionFramework::UI
{
    void Register();
}
