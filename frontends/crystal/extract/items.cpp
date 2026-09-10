// crystal/extract/items.cpp
// Crystal frontend: ItemAttributes table extractor + semanticizer.
//
// The semanticization from Crystal raw HELD_* bytes to engine-semantic
// HeldItemEffectType is the ONLY place in the entire pipeline where Crystal
// HELD_* numeric constants appear.  Nothing downstream (package, reader,
// runtime) ever branches on raw Crystal item or species IDs.
//
// Reference: pokecrystal/data/items/attributes.asm
//            pokecrystal/constants/item_data_constants.asm
//            pokecrystal/engine/battle/effect_commands.asm (BattleCommand_Critical,
//            BattleCommand_CheckHit, BattleCommand_ApplyDamage, etc.)

#include "crystal/extract/item_extractor.hpp"
#include "engine/core/types.hpp"
#include <format>
#include <cstdint>

namespace crystal {

namespace {

// ============================================================================
// Crystal HELD_* raw byte constants (frontend-only; never escape this file)
// Source: pokecrystal/constants/item_data_constants.asm
// ============================================================================
constexpr uint8_t HELD_NONE             =  0;
constexpr uint8_t HELD_BERRY            =  1;
constexpr uint8_t HELD_LEFTOVERS        =  3;
constexpr uint8_t HELD_RESTORE_PP       =  6;
constexpr uint8_t HELD_CLEANSE_TAG      =  8;
constexpr uint8_t HELD_HEAL_POISON      = 10;
constexpr uint8_t HELD_HEAL_FREEZE      = 11;
constexpr uint8_t HELD_HEAL_BURN        = 12;
constexpr uint8_t HELD_HEAL_SLEEP       = 13;
constexpr uint8_t HELD_HEAL_PARALYZE    = 14;
constexpr uint8_t HELD_HEAL_STATUS      = 15;
constexpr uint8_t HELD_HEAL_CONFUSION   = 16;
constexpr uint8_t HELD_METAL_POWDER     = 42;
constexpr uint8_t HELD_NORMAL_BOOST     = 50;
// ...HELD_FIGHTING_BOOST=51 through HELD_STEEL_BOOST=66 (sequential 50-66)
constexpr uint8_t HELD_STEEL_BOOST      = 66;
constexpr uint8_t HELD_ESCAPE           = 72;
constexpr uint8_t HELD_CRITICAL_UP      = 73;
constexpr uint8_t HELD_QUICK_CLAW       = 74;
constexpr uint8_t HELD_FLINCH           = 75;
constexpr uint8_t HELD_AMULET_COIN      = 76;
constexpr uint8_t HELD_BRIGHTPOWDER     = 77;
constexpr uint8_t HELD_FOCUS_BAND       = 79;

// Crystal ItemAttributes struct offsets within each 7-byte record.
// Source: pokecrystal/constants/item_data_constants.asm
constexpr uint8_t ITEMATTR_PRICE_LO  = 0;  // u8 low byte of price
constexpr uint8_t ITEMATTR_PRICE_HI  = 1;  // u8 high byte of price
constexpr uint8_t ITEMATTR_EFFECT    = 2;  // u8 HELD_* constant
constexpr uint8_t ITEMATTR_PARAM     = 3;  // u8 parameter
constexpr uint8_t ITEMATTR_PERMS     = 4;  // u8 permissions (CANT_SELECT, CANT_TOSS)
constexpr uint8_t ITEMATTR_POCKET    = 5;  // u8 pocket type nibble
// Byte 6: dn field_menu, battle_menu (nibble pair) — not used for held-item pipeline

// Crystal permission bits
constexpr uint8_t CANT_TOSS   = 0x80u;  // bit 7

// Crystal item IDs for species-restricted held effects.
// Source: pokecrystal/constants/item_constants.asm
constexpr uint16_t ITEM_LUCKY_PUNCH  = 0x1E;  // Chansey only: +2 crit stage
constexpr uint16_t ITEM_STICK        = 0x69;  // Farfetch'd only: +2 crit stage
constexpr uint16_t ITEM_BERSERK_GENE = 0x98;  // Not in attributes table (HELD_NONE);
                                               // handled by item ID check.

// Crystal species IDs for species-restricted held effects.
// Source: pokecrystal/constants/pokemon_constants.asm
constexpr uint16_t SPECIES_CHANSEY   = 0x47;  // 71 decimal
constexpr uint16_t SPECIES_FARFETCHD = 0x35;  // 53 decimal
constexpr uint16_t SPECIES_DITTO     = 0x54;  // 84 decimal

// Crystal type IDs for type-damage-boost items.
// Source: pokecrystal/constants/type_constants.asm + data/types/type_boost_items.asm
// Maps HELD_NORMAL_BOOST=50..HELD_STEEL_BOOST=66 to their Crystal TypeId.
// Non-sequential because Crystal's type numbering has gaps (BIRD=6, CURSE=19, etc.).
//
// Exact mapping from TypeBoostItems table in type_boost_items.asm:
//   HELD_NORMAL_BOOST=50  → NORMAL(0)      Pink Bow, Polkadot Bow
//   HELD_FIGHTING_BOOST=51 → FIGHTING(1)   Blackbelt
//   HELD_FLYING_BOOST=52  → FLYING(2)      Sharp Beak
//   HELD_POISON_BOOST=53  → POISON(3)      Poison Barb
//   HELD_GROUND_BOOST=54  → GROUND(4)      Soft Sand
//   HELD_ROCK_BOOST=55    → ROCK(5)        Hard Stone
//   HELD_BUG_BOOST=56     → BUG(7)         Silverpowder
//   HELD_GHOST_BOOST=57   → GHOST(8)       Spell Tag
//   HELD_FIRE_BOOST=58    → FIRE(20)       Charcoal
//   HELD_WATER_BOOST=59   → WATER(21)      Mystic Water
//   HELD_GRASS_BOOST=60   → GRASS(22)      Miracle Seed
//   HELD_ELECTRIC_BOOST=61 → ELECTRIC(23)  Magnet
//   HELD_PSYCHIC_BOOST=62 → PSYCHIC(24)    Twistedspoon
//   HELD_ICE_BOOST=63     → ICE(25)        Nevermeltice
//   HELD_DRAGON_BOOST=64  → DRAGON(26)     Dragon Scale (NOTE: Dragon Fang has HELD_NONE)
//   HELD_DARK_BOOST=65    → DARK(27)       Blackglasses
//   HELD_STEEL_BOOST=66   → STEEL(9)       Metal Coat
constexpr enginemon::TypeId kTypeBoostTable[17] = {
     0,  // 50 → NORMAL
     1,  // 51 → FIGHTING
     2,  // 52 → FLYING
     3,  // 53 → POISON
     4,  // 54 → GROUND
     5,  // 55 → ROCK
     7,  // 56 → BUG
     8,  // 57 → GHOST
    20,  // 58 → FIRE
    21,  // 59 → WATER
    22,  // 60 → GRASS
    23,  // 61 → ELECTRIC
    24,  // 62 → PSYCHIC
    25,  // 63 → ICE
    26,  // 64 → DRAGON
    27,  // 65 → DARK
     9,  // 66 → STEEL
};

// Semanticize a single item entry from the 7-byte ROM record.
// item_id is 1-based; the record is already read from the ROM.
static void semanticize_item(
    PackageWriter::ItemDataEntry& e,
    uint16_t item_id,
    const uint8_t* rec)
{
    using HT = enginemon::HeldItemEffectType;

    e.id              = static_cast<enginemon::ItemId>(item_id);
    e.price           = static_cast<uint16_t>(rec[ITEMATTR_PRICE_LO]) |
                        (static_cast<uint16_t>(rec[ITEMATTR_PRICE_HI]) << 8);
    e.held_param      = rec[ITEMATTR_PARAM];
    e.permissions     = rec[ITEMATTR_PERMS];
    e.pocket          = rec[ITEMATTR_POCKET] & 0x0Fu;  // low nibble is pocket

    // Defaults
    e.held_effect_type     = HT::None;
    e.boosted_type         = enginemon::TYPE_NONE;
    e.species_restriction  = enginemon::SPECIES_NONE;
    e.consumable           = false;

    const uint8_t eff = rec[ITEMATTR_EFFECT];  // raw HELD_* byte (Crystal-frontend only)

    // ── Species-restricted items: dispatched by item ID, not HELD_* byte ──
    // LUCKY_PUNCH and BERSERK_GENE have HELD_NONE in the attributes table.
    // STICK also has HELD_NONE. These require item-ID-based special-casing
    // in the compiler (frontend-only). The runtime sees semantic types only.
    if (item_id == ITEM_LUCKY_PUNCH) {
        e.held_effect_type    = HT::CritStageBoostSpecies;
        e.held_param          = 2;  // +2 crit stages (Crystal BattleCommand_Critical)
        e.species_restriction = static_cast<enginemon::SpeciesId>(SPECIES_CHANSEY);
        return;
    }
    if (item_id == ITEM_STICK) {
        e.held_effect_type    = HT::CritStageBoostSpecies;
        e.held_param          = 2;  // +2 crit stages (Crystal BattleCommand_Critical)
        e.species_restriction = static_cast<enginemon::SpeciesId>(SPECIES_FARFETCHD);
        return;
    }
    if (item_id == ITEM_BERSERK_GENE) {
        // BERSERK_GENE has HELD_NONE in attributes — special case by item ID.
        e.held_effect_type = HT::BerserkActivation;
        e.consumable       = true;
        return;
    }

    // ── HELD_* byte dispatch (all Crystal-frontend-only; never escapes this file) ──
    switch (eff) {
    case HELD_NONE:
        // Most items fall here; held_effect_type stays None.
        break;

    case HELD_BERRY:
        // Berry (param=10), Gold Berry (param=30), Berry Juice (param=20).
        e.held_effect_type = HT::EndTurnHealBelowHalf;
        e.consumable = true;
        break;

    case HELD_LEFTOVERS:
        // Restores max_hp/16 each turn. param=10 in ROM (irrelevant to formula).
        e.held_effect_type = HT::EndTurnHealFraction;
        e.held_param = 16;  // denominator: heal max_hp / 16
        e.consumable = false;
        break;

    case HELD_RESTORE_PP:
        // Mysteryberry: restore 5 PP to first depleted move; consumed.
        e.held_effect_type = HT::EndTurnRestorePP;
        e.consumable = true;
        break;

    case HELD_CLEANSE_TAG:
        // Reduces wild encounter rate — overworld only; no battle effect.
        break;

    case HELD_HEAL_POISON:
        // PSNCUREBERRY — cure Poison (Status::Poison) at end of turn.
        // param encodes Status::Poison in the runtime; we store the raw param=0
        // and let the runtime use the Status enum. To avoid Status enum knowledge
        // here, we use a flat param value matching Status::Poison = 3 (engine enum).
        e.held_effect_type = HT::StatusCure;
        e.held_param = 3;  // Status::Poison = 3 in engine
        e.consumable = true;
        break;
    case HELD_HEAL_FREEZE:
        // Burnt Berry — cure Freeze (Status::Freeze = 4).
        e.held_effect_type = HT::StatusCure;
        e.held_param = 4;  // Status::Freeze = 4 in engine
        e.consumable = true;
        break;
    case HELD_HEAL_BURN:
        // Ice Berry — cure Burn (Status::Burn = 2).
        e.held_effect_type = HT::StatusCure;
        e.held_param = 2;  // Status::Burn = 2 in engine
        e.consumable = true;
        break;
    case HELD_HEAL_SLEEP:
        // Mint Berry — wake up (Status::Sleep = 1).
        e.held_effect_type = HT::StatusCure;
        e.held_param = 1;  // Status::Sleep = 1 in engine
        e.consumable = true;
        break;
    case HELD_HEAL_PARALYZE:
        // PRZCureBerry — cure Paralysis (Status::Paralysis = 5).
        e.held_effect_type = HT::StatusCure;
        e.held_param = 5;  // Status::Paralysis = 5 in engine
        e.consumable = true;
        break;
    case HELD_HEAL_STATUS:
        // MiracleBerry — cure any major status and confusion.
        e.held_effect_type = HT::AnyStatusCure;
        e.consumable = true;
        break;
    case HELD_HEAL_CONFUSION:
        // Bitter Berry — cure confusion.
        e.held_effect_type = HT::ConfusionCure;
        e.consumable = true;
        break;

    case HELD_METAL_POWDER:
        // Metal Powder: Ditto only, ×1.5 defense. held_param=10 in ROM (unused).
        e.held_effect_type    = HT::SpeciesDefenseBoost;
        e.species_restriction = static_cast<enginemon::SpeciesId>(SPECIES_DITTO);
        e.consumable = false;
        break;

    case HELD_ESCAPE:
        // Smoke Ball: auto-escape from wild battle. No param.
        e.held_effect_type = HT::GuaranteedEscape;
        break;

    case HELD_CRITICAL_UP:
        // Scope Lens: +1 crit stage. param=0 in ROM (irrelevant); runtime uses BattleRules delta.
        // Store param=1 explicitly so the runtime can apply the delta without BattleRules.
        e.held_effect_type = HT::CritStageBoost;
        e.held_param = 1;  // +1 crit stage (scope_lens_delta from BattleRules)
        break;

    case HELD_QUICK_CLAW:
        // Quick Claw: param=60 (60/256 ≈ 23.4% chance to go first).
        e.held_effect_type = HT::TurnOrderBoost;
        // held_param already = 60 from ROM
        break;

    case HELD_FLINCH:
        // King's Rock: param=30 (30/256 ≈ 11.7% post-hit flinch chance).
        e.held_effect_type = HT::PostHitFlinch;
        // held_param already = 30 from ROM
        break;

    case HELD_AMULET_COIN:
        // Amulet Coin: double trainer prize money and Pay Day payout.
        e.held_effect_type = HT::AmuletCoin;
        break;

    case HELD_BRIGHTPOWDER:
        // BrightPowder: param=20 (subtract 20 from opponent's accuracy before stage mult).
        e.held_effect_type = HT::AccuracyReduction;
        // held_param already = 20 from ROM
        break;

    case HELD_FOCUS_BAND:
        // Focus Band: param=30 (30/256 chance to survive KO at 1 HP; not consumed).
        e.held_effect_type = HT::KOSurvival;
        // held_param already = 30 from ROM
        e.consumable = false;
        break;

    default:
        // Type-boost items: HELD_NORMAL_BOOST(50) through HELD_STEEL_BOOST(66).
        if (eff >= HELD_NORMAL_BOOST && eff <= HELD_STEEL_BOOST) {
            const uint8_t idx = eff - HELD_NORMAL_BOOST;
            e.held_effect_type = HT::TypeDamageBoost;
            e.boosted_type     = kTypeBoostTable[idx];
            // held_param already = 10 from ROM (damage × 110/100)
        }
        // All other HELD_* values (unused in stock Crystal) remain None.
        break;
    }
}

} // anonymous namespace

// ============================================================================
// Public interface
// ============================================================================

ItemExtractResult extract_all_items(
    const RomData& rom,
    const ExtractionProfile& profile)
{
    ItemExtractResult result;

    const auto& o   = profile.offsets;
    const auto& fmt = profile.format.item;
    const auto& c   = profile.counts;

    if (o.item_attributes == 0) {
        result.error = "profile.offsets.item_attributes is zero — not configured";
        return result;
    }
    if (c.num_items < 2) {
        result.error = "profile.counts.num_items < 2 — nothing to extract";
        return result;
    }
    if (fmt.attr_size != 7) {
        result.error = std::format(
            "profile.format.item.attr_size == {} expected 7 (ITEMATTR_STRUCT_LENGTH)",
            fmt.attr_size);
        return result;
    }

    // The ROM table has (num_items - 1) entries: items 1..num_items-1.
    // Item 0 (NO_ITEM) has no ROM record and is emitted as a zero-effect entry.
    const uint32_t table_count = c.num_items - 1;
    const uint64_t table_bytes =
        static_cast<uint64_t>(table_count) * static_cast<uint64_t>(fmt.attr_size);

    if (o.item_attributes + table_bytes > rom.size()) {
        result.error = std::format(
            "ItemAttributes table (base={:#x}, count={}, record_size={}) "
            "extends past ROM size {:#x}",
            o.item_attributes, table_count, fmt.attr_size, rom.size());
        return result;
    }

    result.items.reserve(c.num_items);

    // Item 0 (NO_ITEM): emit as a placeholder with all-zero fields.
    {
        PackageWriter::ItemDataEntry e{};
        e.id = static_cast<enginemon::ItemId>(0);
        result.items.push_back(e);
    }

    // Items 1..num_items-1: read from ROM and semanticize.
    for (uint16_t i = 1; i < c.num_items; ++i) {
        const uint32_t entry_addr =
            o.item_attributes + static_cast<uint32_t>(i - 1) * fmt.attr_size;
        const auto rec = rom.read_bytes(entry_addr, fmt.attr_size);

        PackageWriter::ItemDataEntry e{};
        semanticize_item(e, i, rec.data());
        result.items.push_back(e);
    }

    result.success = true;
    return result;
}

} // namespace crystal
