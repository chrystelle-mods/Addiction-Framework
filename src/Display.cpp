#include "Display.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Re-apply only when the largest sub-effect's magnitude has drifted at least this much (points), so we
    // don't remove/re-add the ability every tick — a handful of discrete steps instead.
    constexpr float kMagnitudeStep = 1.0f;

    // Build the single visible carrier's description by listing every sub-effect with its current scaled
    // magnitude — e.g. "Stamina reduced by 50. Health reduced by 50. Magicka reduced by 50." One MGEF can
    // only expand its own <mag>, so the numbers are computed and baked here (refreshed on each drift-apply)
    // rather than left as a <mag> token.
    std::string ComposeDescription(const std::vector<AddictionFramework::SubEffect>& a_effects, bool a_detrimental,
                                   float a_level)
    {
        std::string out;
        for (const auto& se : a_effects) {
            const long n = std::lround(se.max * (a_level / 100.0f));
            out += se.name.empty() ? std::string("Actor value") : se.name;
            out += a_detrimental ? " reduced by " : " increased by ";
            out += std::to_string(n);
            out += ". ";
        }
        if (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        return out;
    }
}

namespace AddictionFramework
{
    EffectManager* EffectManager::GetSingleton()
    {
        static EffectManager singleton;
        return &singleton;
    }

    RE::SpellItem* EffectManager::Resolve(RE::FormID a_spellID, Applied& a_entry)
    {
        if (!a_entry.spell && a_spellID) {
            a_entry.spell = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
        }
        return a_entry.spell;
    }

    void EffectManager::ConfigureEffect(RE::FormID a_spellID, const std::vector<SubEffect>& a_effects,
                                        bool a_detrimental, const std::string& a_name)
    {
        if (!a_spellID) {
            return;
        }
        auto& entry = _effects[a_spellID];
        auto* spell = Resolve(a_spellID, entry);
        if (!spell) {
            logger::error("EffectManager: configure spell {:08X} did not resolve.", a_spellID);
            return;
        }

        entry.effects     = a_effects;
        entry.detrimental = a_detrimental;

        // Name the SPELL after the category effect ("Skooma Addiction") — that's the SOURCE column in the
        // Active Effects menu. Each MGEF below is named after what it does, so a multi-AV effect (skooma
        // withdrawal = Stamina+Health+Magicka) reads as distinct rows that all share this one source.
        if (!a_name.empty()) {
            spell->fullName = a_name.c_str();
        }

        using Flag             = RE::EffectSetting::EffectSettingData::Flag;
        const std::size_t pool = spell->effects.size();
        const std::size_t used = std::min(a_effects.size(), pool);
        for (std::size_t i = 0; i < pool; ++i) {
            auto* eff = spell->effects[i];
            if (!eff || !eff->baseEffect) {
                continue;
            }
            auto* mgef = eff->baseEffect;
            if (i < used) {
                mgef->data.primaryAV = a_effects[i].av;  // retarget the blank to the configured AV (Spike C)
                if (a_detrimental) {
                    mgef->data.flags.set(Flag::kDetrimental);
                } else {
                    mgef->data.flags.reset(Flag::kDetrimental);
                }
                // Collapse to ONE visible row: blank[0] is the carrier (named after the category effect, e.g.
                // "Skooma Withdrawal"), its description lists every sub-effect (set at apply time). The other
                // used blanks still apply their AV but are hidden, so the menu shows a single labeled row per
                // effect instead of one row per actor value.
                if (i == 0) {
                    mgef->data.flags.reset(Flag::kHideInUI);
                    if (!a_name.empty()) {
                        mgef->fullName = a_name.c_str();
                    }
                } else {
                    mgef->data.flags.set(Flag::kHideInUI);
                }
            } else {
                mgef->data.flags.set(Flag::kHideInUI);  // hide the unused blanks
            }
            eff->effectItem.magnitude = 0.0f;  // scaled at apply time
        }
        entry.configured = true;

        if (a_effects.size() > pool) {
            logger::warn("EffectManager: spell {:08X} pool holds {} blank(s) but {} effect(s) configured — "
                         "extra ignored.",
                         a_spellID, pool, a_effects.size());
        }
        logger::info("EffectManager: configured {:08X} '{}' — {} of {} blank(s) used, detrimental={}.", a_spellID,
                     a_name, used, pool, a_detrimental);
    }

    void EffectManager::ApplyEffect(RE::FormID a_spellID, bool a_active, float a_level)
    {
        auto it = _effects.find(a_spellID);
        if (it == _effects.end() || !it->second.configured) {
            return;
        }
        auto& entry  = it->second;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !entry.spell) {
            return;
        }
        auto*      spell = entry.spell;
        const bool has   = player->HasSpell(spell);

        if (!a_active) {
            if (has) {
                player->RemoveSpell(spell);
            }
            entry.applied      = false;
            entry.appliedLevel = -1.0f;
            return;
        }

        // Re-apply on first activation, on loss of the spell, or when the largest sub-effect's magnitude
        // has drifted a meaningful step.
        float maxMax = 0.0f;
        for (auto& se : entry.effects) {
            maxMax = std::max(maxMax, se.max);
        }
        const float magDrift   = std::abs(a_level - entry.appliedLevel) * maxMax / 100.0f;
        const bool  needsApply = !entry.applied || !has || magDrift >= kMagnitudeStep;
        if (!needsApply) {
            return;
        }

        const std::size_t used = std::min<std::size_t>(entry.effects.size(), spell->effects.size());
        for (std::size_t i = 0; i < used; ++i) {
            if (spell->effects[i]) {
                spell->effects[i]->effectItem.magnitude = entry.effects[i].max * (a_level / 100.0f);
            }
        }
        // Refresh the single visible carrier's description with the current numbers (blank[0]).
        if (used > 0 && spell->effects[0] && spell->effects[0]->baseEffect) {
            const std::string desc = ComposeDescription(entry.effects, entry.detrimental, a_level);
            spell->effects[0]->baseEffect->magicItemDescription = desc.c_str();
        }
        if (has) {
            player->RemoveSpell(spell);
        }
        player->AddSpell(spell);
        entry.applied      = true;
        entry.appliedLevel = a_level;
    }

    void EffectManager::ClearAll()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (auto& [id, entry] : _effects) {
            if (player && entry.spell && player->HasSpell(entry.spell)) {
                player->RemoveSpell(entry.spell);
            }
            entry.applied      = false;
            entry.appliedLevel = -1.0f;
        }
    }
}
