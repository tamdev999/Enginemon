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
// XREF: RandomPhoneMon — ld hl,TrainerGroups / ld a,d / dec a / ld c,a / ld b,0 /
//                          add hl,bc × 3 / ld a,BANK
// Pattern: 21 lo hi 7A 3D 4F 06 00 09 09 09 3E bb
ResolvedAddress resolve_trainer_groups(
    const RomData& rom,
    uint32_t profile_address,
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
