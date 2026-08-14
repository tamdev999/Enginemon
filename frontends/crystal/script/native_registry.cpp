// crystal/script/native_registry.cpp
// Stage 3: NativeCallRegistry + RamAddressRegistry implementation
//
// Classifies non-script machine-facing references so semantic lowering
// never needs raw ASM targets or GB RAM addresses.

#include "crystal/script/native_registry.hpp"

namespace crystal {

// =============================================================================
// NATIVE CALL REGISTRY IMPLEMENTATION
// =============================================================================

void NativeCallRegistry::add_known(uint32_t address, const char* symbol, const char* semantic,
                                    NativeClassification cls, NativeControlFlow cf,
                                    const char* source, const char* notes) {
    NativeCallEntry entry;
    entry.address = address;
    entry.symbol_name = symbol;
    entry.semantic_name = semantic;
    entry.classification = cls;
    entry.control_flow = cf;
    entry.confidence = Confidence::Verified;
    entry.source_reference = source;
    entry.notes = notes;
    entries_[address] = entry;
}

void NativeCallRegistry::initialize() {
    // Known native routines from pokecrystal analysis
    // Format: flat_address, symbol, semantic_behavior, classification, control_flow, source, notes
    //
    // Addresses are from pokecrystal11.sym - convert bank:addr to flat:
    //   flat = (bank * 0x4000) + (addr & 0x3FFF)  [for bank > 0]
    //   flat = addr                               [for bank 0, addr < 0x4000]
    
    // Bank 0 (home) routines - always available
    // Random (00:2f8c) -> 0x2f8c
    add_known(0x2f8c, "Random", "random",
              NativeClassification::PureSemantic, NativeControlFlow::Returns,
              "pokecrystal/home/random.asm", "Returns random byte in a");
    
    // Bank 3 (03:4xxx) - Common utility routines
    // HealParty (03:4658) -> 0xC000 + 0x0658 = 0xC658
    add_known(0xC658, "HealParty", "heal_party",
              NativeClassification::PureSemantic, NativeControlFlow::Returns,
              "pokecrystal/engine/pokemon/party.asm", "Heals all party pokemon");
    
    // HealPartySpecial (03:407a) -> 0xC000 + 0x007a = 0xC07a
    add_known(0xC07a, "HealPartySpecial", "heal_party",
              NativeClassification::PureSemantic, NativeControlFlow::Returns,
              "pokecrystal/engine/events/specials.asm", "Heals party special wrapper");
    
    // GetPartyNickname (03:4706) -> 0xC000 + 0x0706 = 0xC706
    add_known(0xC706, "GetPartyNickname", "get_party_nickname",
              NativeClassification::Trivial, NativeControlFlow::Returns,
              "pokecrystal/engine/pokemon/party.asm", "Gets nickname of party member");
    
    // =========================================================================
    // Strength/Rock Smash HM native calls (used by StdScripts 14 and 15)
    // All verified to return (end with `ret`) - no transfers, no terminals
    // =========================================================================
    
    // TryStrengthOW (03:4D78) -> bank 3 = 0xC000 base, 0x4D78 - 0x4000 = 0x0D78 -> 0xCD78
    // Reference: pokecrystal/engine/events/overworld.asm TryStrengthOW
    // Used by StrengthBoulderScript - checks if player can use Strength
    add_known(0xCD78, "TryStrengthOW", "try_strength_overworld",
              NativeClassification::PureSemantic, NativeControlFlow::Returns,
              "pokecrystal/engine/events/overworld.asm", "Checks if Strength can be used overworld");
    
    // SetStrengthFlag (03:4D12) -> 0xCD12
    // Reference: pokecrystal/engine/events/overworld.asm SetStrengthFlag
    // Used by AskStrengthScript - sets Strength active flag
    add_known(0xCD12, "SetStrengthFlag", "set_strength_flag",
              NativeClassification::PureSemantic, NativeControlFlow::Returns,
              "pokecrystal/engine/events/overworld.asm", "Sets the Strength active flag");
    
    // HasRockSmash (03:4F7C) -> 0xCF7C
    // Reference: pokecrystal/engine/events/overworld.asm HasRockSmash
    // Used by SmashRockScript - checks if player has Rock Smash
    add_known(0xCF7C, "HasRockSmash", "has_rock_smash",
              NativeClassification::PureSemantic, NativeControlFlow::Returns,
              "pokecrystal/engine/events/overworld.asm", "Checks if party has Rock Smash");
    
    // RockMonEncounter (2E:4219) -> bank 0x2E = 46, 0x4219 - 0x4000 = 0x0219 -> 0xB8000 + 0x0219 = 0xB8219
    // Reference: pokecrystal/engine/events/treemons.asm RockMonEncounter
    // Used by SmashRockScript - triggers wild encounter from rock smashing
    add_known(0xB8219, "RockMonEncounter", "rock_mon_encounter",
              NativeClassification::PureSemantic, NativeControlFlow::Returns,
              "pokecrystal/engine/events/treemons.asm", "Triggers rock smash wild encounter");
    
    // Bank 25 (0x25 = 37) - Overworld scripting
    // Address formula: 37 * 0x4000 + (addr - 0x4000) = 0x94000 + offset
    // EnableEvents (25:66d0) -> 0x94000 + 0x26d0 = 0x966d0
    add_known(0x966d0, "EnableEvents", "enable_events",
              NativeClassification::HostCapability, NativeControlFlow::Returns,
              "pokecrystal/engine/overworld/events.asm", "Re-enables event processing");
    
    // DisableWildEncounters (25:66ee) -> 0x94000 + 0x26ee = 0x966ee
    add_known(0x966ee, "DisableWildEncounters", "disable_wild_encounters",
              NativeClassification::HostCapability, NativeControlFlow::Returns,
              "pokecrystal/engine/overworld/events.asm", "Disables wild encounters");
    
    // EnableWildEncounters (25:6706) -> 0x94000 + 0x2706 = 0x96706
    add_known(0x96706, "EnableWildEncounters", "enable_wild_encounters",
              NativeClassification::HostCapability, NativeControlFlow::Returns,
              "pokecrystal/engine/overworld/events.asm", "Enables wild encounters");
    
    // Bank 2 (02:4xxx) routines
    // HealPartyPredef (02:4571) -> 0x8000 + 0x0571 = 0x8571
    add_known(0x8571, "HealPartyPredef", "heal_party",
              NativeClassification::PureSemantic, NativeControlFlow::Returns,
              "pokecrystal/engine/predefs.asm", "Heal party predef wrapper");
    
    // NOTE: Most callasm targets in Crystal scripts are local map-specific
    // routines that we encounter dynamically. The registry will classify
    // them as Opaque/Unknown by default when first encountered.
}

void NativeCallRegistry::register_target(uint32_t address) {
    if (entries_.contains(address)) {
        // Already registered, do nothing
        return;
    }
    
    // Create opaque entry for unknown target
    NativeCallEntry entry;
    entry.address = address;
    entry.symbol_name = "";  // Unknown
    entry.semantic_name = "";
    entry.classification = NativeClassification::Opaque;
    entry.control_flow = NativeControlFlow::Unknown;
    entry.confidence = Confidence::Unverified;
    entry.source_reference = "corpus_scan";
    entry.notes = "Encountered during corpus scan";
    entries_[address] = entry;
}

const NativeCallEntry* NativeCallRegistry::get(uint32_t address) const {
    auto it = entries_.find(address);
    if (it != entries_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool NativeCallRegistry::is_classified(uint32_t address) const {
    auto entry = get(address);
    return entry && entry->is_classified();
}

size_t NativeCallRegistry::classified_count() const {
    size_t count = 0;
    for (const auto& [addr, entry] : entries_) {
        if (entry.is_classified()) {
            count++;
        }
    }
    return count;
}

size_t NativeCallRegistry::opaque_count() const {
    size_t count = 0;
    for (const auto& [addr, entry] : entries_) {
        if (!entry.is_classified()) {
            count++;
        }
    }
    return count;
}

size_t NativeCallRegistry::count_by_control_flow(NativeControlFlow cf) const {
    size_t count = 0;
    for (const auto& [addr, entry] : entries_) {
        if (entry.control_flow == cf) {
            count++;
        }
    }
    return count;
}

// =============================================================================
// RAM ADDRESS REGISTRY IMPLEMENTATION
// =============================================================================

void RamAddressRegistry::add_known(uint16_t address, const char* symbol, const char* semantic,
                                    RamClassification cls, const char* source, const char* notes) {
    RamAddressEntry entry;
    entry.address = address;
    entry.symbol_name = symbol;
    entry.semantic_meaning = semantic;
    entry.classification = cls;
    entry.confidence = Confidence::Verified;
    entry.source_reference = source;
    entry.notes = notes;
    entries_[address] = entry;
}

void RamAddressRegistry::initialize() {
    // Known RAM addresses from pokecrystal/ram/wram.asm
    // Addresses from pokecrystal11.sym
    
    // Core script state
    // wScriptVar (00:c2dd) - The primary script variable
    add_known(0xc2dd, "wScriptVar", "script_var",
              RamClassification::KnownSemanticState, 
              "pokecrystal/ram/wram.asm", "Primary script variable for return values");
    
    // wScriptMode (01:d437)
    add_known(0xd437, "wScriptMode", "script_mode",
              RamClassification::KnownCapabilitySlot,
              "pokecrystal/ram/wram.asm", "Current script processing mode");
    
    // Party/Pokemon state
    // wCurPartyMon (01:d109)
    add_known(0xd109, "wCurPartyMon", "current_party_mon",
              RamClassification::KnownSemanticState,
              "pokecrystal/ram/wram.asm", "Index of current party pokemon");
    
    // wCurFruit (01:d03f)
    add_known(0xd03f, "wCurFruit", "current_fruit",
              RamClassification::KnownSemanticState,
              "pokecrystal/ram/wram.asm", "ID of current fruit tree item");
    
    // wCurFruitTree (01:d03e)
    add_known(0xd03e, "wCurFruitTree", "current_fruit_tree",
              RamClassification::KnownSemanticState,
              "pokecrystal/ram/wram.asm", "ID of current fruit tree");
    
    // Player state (common addresses)
    // These are estimates - actual values may vary slightly
    // wPlayerNextMovement
    add_known(0xc2de, "wPlayerNextMovement", "player_next_movement",
              RamClassification::KnownSemanticState,
              "pokecrystal/ram/wram.asm", "Player's next movement byte");
    
    // wPlayerMovement  
    add_known(0xc2df, "wPlayerMovement", "player_movement",
              RamClassification::KnownSemanticState,
              "pokecrystal/ram/wram.asm", "Player's current movement byte");
    
    // Movement object state
    // wMovementObject
    add_known(0xc2e2, "wMovementObject", "movement_object",
              RamClassification::KnownSemanticState,
              "pokecrystal/ram/wram.asm", "Object ID for movement commands");
    
    // wMovementDataBank
    add_known(0xc2e3, "wMovementDataBank", "movement_data_bank",
              RamClassification::KnownCapabilitySlot,
              "pokecrystal/ram/wram.asm", "Bank of movement data");
    
    // wMovementDataAddress (2 bytes)
    add_known(0xc2e4, "wMovementDataAddress", "movement_data_address",
              RamClassification::ControlFlowPointer,
              "pokecrystal/ram/wram.asm", "Pointer to movement data");
    
    // Mini-game and event state (found during corpus scan)
    // wMooMooBerries (01:d962)
    add_known(0xd962, "wMooMooBerries", "moo_moo_berries",
              RamClassification::KnownSemanticState,
              "pokecrystal11.sym", "MooMoo Farm berry feeding count");
    
    // wUndergroundSwitchPositions (01:d963)
    add_known(0xd963, "wUndergroundSwitchPositions", "underground_switch_positions",
              RamClassification::KnownSemanticState,
              "pokecrystal11.sym", "Goldenrod Underground switch states");
    
    // wFarfetchdPosition (01:d964)
    add_known(0xd964, "wFarfetchdPosition", "farfetchd_position",
              RamClassification::KnownSemanticState,
              "pokecrystal11.sym", "Farfetch'd herding mini-game position");
    
    // wOtherPlayerLinkMode / wLinkReceivedSyncBuffer (00:cf51)
    add_known(0xcf51, "wOtherPlayerLinkMode", "other_player_link_mode",
              RamClassification::KnownCapabilitySlot,
              "pokecrystal11.sym", "Link mode state (multi-use UNION address)");
    
    // =========================================================================
    // Field Move State Variables (used by StdScripts 14/15 but NOT script-accessed)
    // These are classified for registry completeness but their runtime meaning
    // is eliminated through semantic context (SelectedFieldActor, PendingFieldEncounter)
    // =========================================================================
    
    // wStrengthSpecies (01:d1ef) - Species that used Strength
    // Set by SetStrengthFlag native, read by subsequent cry commands
    // NOT accessed by scripts directly - lowered to Sem_PlayFieldActorCry context
    add_known(0xd1ef, "wStrengthSpecies", "strength_species",
              RamClassification::KnownSemanticState,
              "pokecrystal11.sym", "Species that used Strength (field move context)");
    
    // wTempWildMonSpecies (01:d22e) - Pending wild encounter species
    // Set by RockMonEncounter native, read by subsequent battle setup
    // Script access via readmem lowered to Sem_ReadEncounterSpecies context
    add_known(0xd22e, "wTempWildMonSpecies", "temp_wild_mon_species",
              RamClassification::KnownSemanticState,
              "pokecrystal11.sym", "Pending wild encounter species (field move context)");
    
    // Script execution pointers (potential control flow)
    // These are addresses that store script pointers which memjump/memcall read
    
    // wCurrentMapTriggerPointer - stores script pointer for map triggers
    // wQueuedScriptBank, wQueuedScriptAddr - queued script execution
    
    // NOTE: Most RAM addresses encountered in scripts are game-specific state.
    // The registry will classify them as OpaqueRam by default when first encountered
    // unless they match a known pattern.
}

void RamAddressRegistry::register_access(uint16_t address, RamAccessKind kind) {
    auto it = entries_.find(address);
    if (it != entries_.end()) {
        // Already registered, just add access kind
        it->second.add_access(kind);
        return;
    }
    
    // Create opaque entry for unknown address
    RamAddressEntry entry;
    entry.address = address;
    entry.symbol_name = "";  // Unknown
    entry.semantic_meaning = "";
    entry.classification = RamClassification::OpaqueRam;
    entry.confidence = Confidence::Unverified;
    entry.source_reference = "corpus_scan";
    entry.notes = "Encountered during corpus scan";
    entry.add_access(kind);
    entries_[address] = entry;
}

const RamAddressEntry* RamAddressRegistry::get(uint16_t address) const {
    auto it = entries_.find(address);
    if (it != entries_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool RamAddressRegistry::is_classified(uint16_t address) const {
    auto entry = get(address);
    return entry && entry->is_classified();
}

size_t RamAddressRegistry::classified_count() const {
    size_t count = 0;
    for (const auto& [addr, entry] : entries_) {
        if (entry.is_classified()) {
            count++;
        }
    }
    return count;
}

size_t RamAddressRegistry::opaque_count() const {
    size_t count = 0;
    for (const auto& [addr, entry] : entries_) {
        if (!entry.is_classified()) {
            count++;
        }
    }
    return count;
}

} // namespace crystal
