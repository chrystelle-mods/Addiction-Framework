#include "Casting.h"

#include "Addiction.h"

namespace
{
    // Cast/consume trigger. The engine implements eat/drink as "casting" the ingestible on the player, so
    // BOTH consumption AND spell-casting arrive here as one TESSpellCastEvent whose `spell` is the cast
    // MagicItem's FormID (AlchemyItem for a potion/food, SpellItem for a spell). This catches REAL
    // inventory-menu drinks — which the Actor::DrinkPotion vtable hook missed (proven in-game 2026-08-15).
    //   - AlchemyItem with an ENIT addictionItem → relay a use to that category (the consumption trigger).
    //   - SpellItem → log the school (the casting trigger's foundation; spell→category relay is future).
    class CastListener : public RE::BSTEventSink<RE::TESSpellCastEvent>
    {
    public:
        static CastListener* GetSingleton()
        {
            static CastListener singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent*                 a_event,
                                              RE::BSTEventSource<RE::TESSpellCastEvent>* /*a_source*/) override
        {
            if (!a_event || !a_event->object || !a_event->object->IsPlayerRef()) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Consumption: a potion/food carrying an ENIT addictionItem → relay the use (potency = chance).
            if (auto* alch = RE::TESForm::LookupByID<RE::AlchemyItem>(a_event->spell)) {
                if (auto* addict = alch->data.addictionItem) {
                    const float chance = alch->data.addictionChance;
                    AddictionFramework::AddictionManager::GetSingleton()->RegisterUse(addict->GetFormID(), chance);
                    logger::info("Consume(player): '{}' addictionItem=[{:08X}] chance={}", alch->GetName(),
                                 addict->GetFormID(), chance);
                }
                return RE::BSEventNotifyControl::kContinue;
            }

            // Casting: log the school (foundation for the casting trigger; spell→category relay is future).
            if (auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_event->spell)) {
                const RE::ActorValue school = spell->GetAssociatedSkill();
                auto*                avl    = RE::ActorValueList::GetSingleton();
                const char*          school_name =
                    (avl && school != RE::ActorValue::kNone) ? avl->GetActorValueName(school) : "None";
                logger::info("CastingSpike: player cast '{}' [{:08X}] school={} ({})", spell->GetName(),
                             a_event->spell, school_name ? school_name : "?", static_cast<std::int32_t>(school));
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
}

namespace AddictionFramework::CastingSpike
{
    void Install()
    {
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESSpellCastEvent>(CastListener::GetSingleton());
            logger::info("CastingSpike: TESSpellCastEvent sink installed.");
        }
    }
}
