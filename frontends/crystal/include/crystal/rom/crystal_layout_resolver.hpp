#pragma once
// frontends/crystal/include/crystal/rom/crystal_layout_resolver.hpp
//
// Crystal-family generic table/routine discovery layer.
//
// PURPOSE
//   Locate Crystal-family ROM structures without relying on vanilla absolute
//   addresses.  Each resolver uses one or more of these evidence types:
//
//   XREF    — decode LD/CALL operands at a known code pattern to extract the
//             address the routine itself loads.  The pattern is structural
//             (opcode shape), not content-dependent.
//   STRUCTURAL — validate the resolved structure by its format invariants
//             (record stride, sentinel byte, pointer range).
//   CROSS   — validate that a candidate is consistent with another already-
//             resolved structure (e.g. warp targets within known group range).
//
//   Content anchors (Pound stats, type-value ceilings, specific game data) are
//   intentionally avoided.  They are documented where they appear and labelled
//   as HINT-ONLY so they can never alone define a table bound.
//
// DESIGN RULES
//   - Each resolver returns 0 (failure) or the flat ROM address (success).
//   - Hard failure (ambiguous / multiple non-distinguishable candidates)
//     returns 0 and sets out_diagnostic.
//   - No silent fallback to a vanilla absolute address.
//   - Resolvers are stateless; they operate on (rom, profile) inputs only.
//
// XREF PATTERNS USED
//   StdScripts:    [5F 16 00 21 lo hi 19 19 06 bb] — StdScript dispatch
//   BaseData:      [3E sz 21 lo hi DF 11 wl wh 01 sz 00 3E bb CD] — GetBaseData
//   Moves:         [3D 21 lo hi 01 sz 00 DF|E7 3E bb CD|CF] — GetFixedMoveStruct
//   TrainerGroups: [21 lo hi 7A 3D 4F 06 00 09 09 09 3E bb] — RandomPhoneMon
//   TypeMatchups:  [21 i_lo i_hi FA ?? ?? FE ?? 28 ?? 21 t_lo t_hi 2A] — type branch
//   ScriptCmdTable:[CD ?? ?? CD ?? ?? then N×dw ptrs] — RunScriptCommand dispatch

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include <string>

namespace crystal {

// ============================================================================
// Resolution result
// ============================================================================
struct ResolvedAddress {
    uint32_t flat = 0;        // 0 = not found
    std::string source;       // human-readable evidence summary
    bool ambiguous = false;   // multiple non-distinguishable candidates found
};

// ============================================================================
// Individual table/routine resolvers
//
// Each function scans the ROM for its XREF pattern, validates the extracted
// address structurally, and returns the flat address.
//
// If the profile already has a non-zero address, it is validated first; the
// scan is only run if validation fails or address is 0.
// ============================================================================

// Locate StdScripts table.
// XREF: StdScript dispatch routine — ld e,a / ld d,0 / ld hl,StdScripts / add hl,de / add hl,de / ld b,BANK
// Pattern: 5F 16 00 21 lo hi 19 19 06 bb
// Returns flat address of table start. Also sets out_entry_size (2=dw, 3=dba).
ResolvedAddress resolve_std_scripts(
    const RomData& rom,
    uint32_t profile_address,
    uint8_t* out_entry_size = nullptr,
    std::string* out_diagnostic = nullptr);

// Locate BaseData table.
// XREF: _GetBaseData — ld a,BASE_DATA_SIZE / ld hl,BaseData / rst AddNTimes /
//                       ld de,wCurBaseData / ld bc,size / ld a,BANK / call FarCopyBytes
// Pattern: 3E sz 21 lo hi [D7|DF|E7] 11 wl wh 01 sz 00 3E bb CD
// Also extracts BASE_DATA_SIZE into *out_record_size.
ResolvedAddress resolve_base_data(
    const RomData& rom,
    uint32_t profile_address,
    uint8_t* out_record_size = nullptr,
    std::string* out_diagnostic = nullptr);

// Locate Moves table.
// XREF: GetFixedMoveStruct — dec a / ld hl,Moves / ld bc,MOVE_LENGTH / rst AddNTimes /
//                              ld a,BANK / call FarCopyBytes
// Pattern: 3D 21 lo hi 01 sz 00 [D7|DF|E7] 3E bb [CD|CF]
// Also extracts MOVE_LENGTH into *out_record_size.
ResolvedAddress resolve_moves(
    const RomData& rom,
    uint32_t profile_address,
    uint8_t* out_record_size = nullptr,
    std::string* out_diagnostic = nullptr);

// Locate TrainerGroups table.
// XREF: ReadTrainerParty / RandomPhoneMon dispatch (dw table, stride = 2 bytes).
//   ld hl,TrainerGroups / ld a,d / dec a / ld c,a / ld b,0 / add hl,bc ×2 / ld a,BANK
// Pattern: 21 lo hi 7A 3D 4F 06 00 09 09 3E bb   (two 0x09, NOT three)
ResolvedAddress resolve_trainer_groups(
    const RomData& rom,
    uint32_t profile_address,
    std::string* out_diagnostic = nullptr);

// Derive num_trainer_classes from the GetTrainerPic bounds check.
// Pattern: FA ?? ?? A7 C8 FE NN D0
//   = ld a,[wTrainerClass] / and a / ret z / cp NN / ret nc
// where NN = NUM_TRAINER_CLASSES + 1 → num_trainer_classes = NN - 1.
// Exactly one unique NN is required; zero or conflicting hits → returns 0.
// Crystal v1.1: cp 0x44 (68) → 67.  Gold/Silver: cp 0x43 (67) → 66.
uint16_t resolve_num_trainer_classes(
    const RomData& rom,
    std::string* out_diagnostic = nullptr);

// Locate TypeMatchups table.
// XREF: Type effectiveness branch — ld hl,InvTypeMatchups / ld a,[wBattleType] /
//         cp BATTLETYPE_INVERSE / jr z / ld hl,TypeMatchups / ld a,[hli]
// Pattern: 21 i_lo i_hi FA ?? ?? FE ?? 28 ?? 21 t_lo t_hi 2A FE FF
// Also locates InverseTypeMatchups if out_inverse_flat != nullptr.
//
// NOTE: Multiplier set validation uses {0,5,8,10,16,20,32} — the union of
// vanilla Crystal {0,5,20} and Polished Crystal {0,8,16,32} — so this scanner
// is generic across both. Multiplier values outside this set terminate the scan.
ResolvedAddress resolve_type_matchups(
    const RomData& rom,
    uint32_t profile_address,
    uint32_t* out_inverse_flat = nullptr,
    std::string* out_diagnostic = nullptr);

// Locate ScriptCommandTable (inline jump table after RunScriptCommand dispatch).
// XREF: RunScriptCommand — call GetScriptByte / call StackJumpTable / .Jumptable: dw ...
// Pattern: CD ?? ?? CD ?? ?? [N×valid bank-local 2-byte ptrs starting at offset +6]
// Returns flat address of the table (immediately after the two call instructions).
ResolvedAddress resolve_script_command_table(
    const RomData& rom,
    uint32_t profile_address,
    std::string* out_diagnostic = nullptr);

// ============================================================================
// Layout constant resolvers (ROM-xref based, no profile fallback needed)
//
// These extract numeric constants directly from compiled SM83 code in the
// ROM's home bank.  The constants are structural (not content), so they work
// on relocated code without any profile hint.
// ============================================================================

// Derive SCENE_SCRIPT_SIZE (bytes per scene-script entry in MapScriptHeader).
// XREF: map-loading routine in home bank.
//   Vanilla pattern (home bank, 2A 01 NN 00 CD): scene_size=4
//   Polished pattern (home bank, 2A 01 NN 00 DF): scene_size=2
// Returns 0 if the pattern is not found.
uint8_t resolve_scene_script_size(const RomData& rom);

// Derive MAP_LENGTH (bytes per MapGroup entry, i.e. map_entry_size).
// XREF: GetAnyMapPointer in home bank — "dec c / ld b,0 / ld a,MAP_LENGTH / rst".
//   Vanilla: 0D 06 00 3E NN DF → MAP_LENGTH=9
//   Polished: same pattern → MAP_LENGTH=7
// Returns 0 if the pattern is not found.
uint8_t resolve_map_entry_stride(const RomData& rom);

// Derive COORD_EVENT_SIZE (bytes per coord-event entry).
// XREF: map-events counting loop in home bank.
//   Vanilla: C8 01 NN 00 CD → COORD_EVENT_SIZE=8
//   Polished: 3D 01 NN 00 DF → COORD_EVENT_SIZE=5
// Returns 0 if the pattern is not found.
uint8_t resolve_coord_event_size(const RomData& rom);

// Derive block-data encoding mode from the ChangeMap routine in the home bank.
//
// Two mutually exclusive structural patterns are recognised.  Both require three
// consecutive WRAM addresses (blockBank, ptrLo, ptrHi) with the same WRAM hi byte
// and sequential lo bytes (W, W+1, W+2), hi byte in [0xC0, 0xDF].
//
// Pattern RawBytes (Gold/Silver/Crystal):
//   FA lo hi  D7   FA lo+1 hi  5F   FA lo+2 hi  57
//   = ld a,[blockBank] / rst Bankswitch
//     / ld a,[ptrLo] / ld e,a
//     / ld a,[ptrHi] / ld d,a
//   → bank switch immediately before building the block-data pointer into DE.
//     Subsequent loop "1A 13 22" (ld a,[DE] / inc DE / ld [HL+],a) is raw byte copy.
//
// Pattern LZCompressed (Polished Crystal):
//   FA lo hi  47   21 lo+1 hi   2A 66 6F
//   = ld a,[blockBank] / ld b,a
//     / ld hl,[blockPtr] / read 2-byte ptr into HL
//   → bank stored in B (FarDecompressInB calling convention), not directly switched.
//     Call to FarDecompressInB follows.
//
// No ROM-specific addresses.  Returns BlockDataEncoding::Unknown if:
//   - neither pattern matches (0 hits of either kind)
//   - both patterns match (internally contradictory ROM — ambiguous)
//   - multiple distinct hits of the same kind (ambiguous)
//
// This is a global-per-ROM property: there is exactly one ChangeMap routine and
// it applies to all map groups uniformly.
MapFormatRules::BlockDataEncoding resolve_block_data_encoding(const RomData& rom);

// Derive the environment domain maximum (max_environment_value).
// XREF: Environment dispatch routines in home bank.
//
// In both vanilla and Polished Crystal, the home bank contains exactly four
// consecutive environment-direction dispatch routines.  Each follows the pattern:
//
//   call SomePrep      ; CD ?? ??   (prep routine, same target across all four)
//   ret nz             ; C0
//   ld a, [wCurEnvN]   ; FA lo hi   (one of the four directional env WRAM vars)
//   and ENV_MASK       ; E6 NN      ← NN is the max environment value
//   cp ENV_CONST       ; FE zz
//
// ENV_MASK = $07 in all known Crystal-family ROMs, giving a 3-bit domain [0,7].
// The mask value NN IS the authoritative maximum: environment values above NN wrap
// silently (env=8 → 0 after AND $07), so they are semantically out of range.
//
// Pattern to find: CD ?? ?? C0 FA ?? ?? E6 NN FE   (home bank only)
// at least 2 consecutive occurrences with the same NN value.
//
// Returns NN (the mask = max environment value), or 0 if not found.
// Ambiguous (multiple different mask values found) returns 0.
uint8_t resolve_environment_domain(const RomData& rom);

// Derive the MapAttributes bank for ROMs that omit the bank byte from each map entry
// (MapFormatRules::attr_bank_in_entry == false).
//
// XREF: GetMapAttrBank routine in home bank.
// In Polished Crystal, a dedicated routine selects the bank for MapAttributes access:
//
//   ld a, [wCurMap]      ; FA lo hi
//   ld b, a              ; 47
//   ld a, [wCurMapGroup] ; FA lo hi
//   ld c, a              ; 4F
//   ld a, ATTR_BANK      ; 3E NN   ← NN is the MapAttributes bank (hardcoded literal)
//   rst BankedCall        ; CF
//   ret                  ; C9
//
// The literal NN in "3E NN CF C9" is the MapAttributes bank constant.
//
// Pattern: FA ?? ?? 47 FA ?? ?? 4F 3E NN CF C9   (home bank only)
// Must be unique (exactly one match).
//
// Returns NN (the bank byte), or 0xFF (invalid) if not found / ambiguous.
// Vanilla Crystal (attr_bank_in_entry == true) does not use this pattern; returns 0xFF.
uint8_t resolve_map_attr_bank(const RomData& rom);

// ============================================================================
// Group-level MapAttributes bank resolver
//
// For ROMs where map entries carry no explicit attr_bank byte
// (MapFormatRules::attr_bank_in_entry == false, resolve_attr_bank_by_scan == true),
// determines the correct MapAttributes bank for each map group by group-level
// consistency proof:
//
//   For each group G (1..num_map_groups):
//     Count = exact entry count derived from MapGroupPointers[G+1] - MapGroupPointers[G]
//             divided by map_entry_size.
//     For each candidate bank B (1..max_bank):
//       Score = number of group G entries whose attr_ptr at bank B passes the
//               full 9-point structural validity test:
//                 h/w > 0 (zero dimensions always invalid — no per-axis maximum applied),
//                 blockdata_bank < 128,
//                 blockdata_ptr in banked range,
//                 script_bank < 128, script_ptr in valid range,
//                 MapScriptHeader: scene_count <= 30,
//                                 callback_count <= 20,
//                                 warp_count <= 50.
//     If exactly one bank scores == Count: that is the proven attr_bank for group G.
//     If no bank scores == Count but one bank has the unique highest score: fail
//       (ambiguous — do not guess).
//     If zero banks score > 0: fail (not found).
//
// This replaces the per-map minimum-area heuristic scan entirely.
// Vanilla Crystal (attr_bank_in_entry == true) is not affected; the function
// returns immediately with resolved=0 for those profiles.
//
// Results are stored in profile.offsets.group_attr_banks[1..num_map_groups].
// Returns the number of groups successfully resolved.
// ============================================================================
int resolve_group_attr_banks(const RomData& rom, ExtractionProfile& profile,
                              bool verbose = false);

// ============================================================================
// Composite resolver
//
// Runs all individual resolvers and populates any zero fields in the profile.
// Only fills fields that are currently 0 (does not overwrite explicit profile
// addresses). Updates format fields (record sizes) where discovered.
//
// Reports each resolver's result to stderr if verbose=true.
// Returns the number of addresses successfully resolved.
// ============================================================================
int resolve_crystal_layout(
    const RomData& rom,
    ExtractionProfile& profile,
    bool verbose = true);

}  // namespace crystal
