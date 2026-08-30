// frontends/crystal/save/crystal_party_codec.cpp
//
// Crystal party region decode / encode.
//
// Field layout authority:
//   macros/ram.asm, constants/pokemon_data_constants.asm
//
// Endianness facts (all source-verified from pokecrystal):
//   EXP (3 bytes)    : big-endian   — GiveExp stores hMultiplicand[0..2] sequentially
//   Stats (u16 each) : big-endian   — CalcMonStats: [de]=hMultiplicand[1], [de+1]=[2]
//   DVs (u16)        : little-endian — native SM83 dw
//   Stat EXP (u16)   : little-endian — CalcMonStatC reads [hld]/[hl] as LE
//   OT_ID (u16)      : little-endian — native SM83 dw
//   HP/MaxHP (u16)   : big-endian   — same as other stats (party_struct extension)
//
// Note: suiCune serialize.c uses TY_U16LE for stats — that is a suiCune bug
// (same class as the wGameTimeHours TY_U16LE→TY_U16BE fix applied 2026-08).
// Enginemon uses the source-correct big-endian encoding for all stat fields.

#include "crystal_party_codec.hpp"
#include "crystal_sram_layout.hpp"
#include "crystal_save_reader.hpp"  // decode_crystal_string_to / encode_crystal_string_to

#include <format>
#include <stdexcept>

namespace crystal {

using namespace sram_layout;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(
        static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}

static uint16_t read_u16_be(const uint8_t* p) {
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(p[0]) << 8) | static_cast<uint32_t>(p[1]));
}

static void write_u16_le(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

static void write_u16_be(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

// Decode one party_struct (48 bytes) from base pointer.
static CrystalPartyMon decode_one_mon(
    const uint8_t* s,           // pointer to start of party_struct
    const uint8_t* ot_name,     // pointer to OT name bytes (NAME_LENGTH = 11)
    const uint8_t* nickname,    // pointer to nickname bytes (MON_NAME_LENGTH = 11)
    uint8_t slot_idx,
    const PartyCodecDomain& domain)
{
    CrystalPartyMon m;

    m.species = s[MON_OFF_SPECIES];

    // Species validation: 0 = invalid; >num_pokemon and !=254 (EGG) = invalid
    if (m.species == 0) {
        throw SaveImportError(std::format(
            "decode_party: slot {} species is 0 (empty slot in active party)", slot_idx));
    }
    if (m.species != 254 && static_cast<uint16_t>(m.species) > domain.num_pokemon) {
        throw SaveImportError(std::format(
            "decode_party: slot {} species 0x{:02X} exceeds profile num_pokemon {}",
            slot_idx, m.species, domain.num_pokemon));
    }

    m.item = s[MON_OFF_ITEM];
    if (m.item != 0 && static_cast<uint16_t>(m.item) >= domain.num_items) {
        throw SaveImportError(std::format(
            "decode_party: slot {} item 0x{:02X} exceeds profile num_items {}",
            slot_idx, m.item, domain.num_items));
    }

    // Moves and PP
    for (uint32_t i = 0; i < NUM_MOVES; ++i) {
        uint8_t move_id = s[MON_OFF_MOVES + i];
        if (move_id != 0 && static_cast<uint16_t>(move_id) > domain.num_moves) {
            throw SaveImportError(std::format(
                "decode_party: slot {} move[{}] 0x{:02X} exceeds profile num_moves {}",
                slot_idx, i, move_id, domain.num_moves));
        }
        m.moves[i] = move_id;

        uint8_t pp_byte = s[MON_OFF_PP + i];
        m.pp_ups[i] = static_cast<uint8_t>((pp_byte & PP_UP_MASK) >> 6);
        m.pp[i]     = static_cast<uint8_t>(pp_byte & PP_MASK);
    }

    m.ot_id = read_u16_le(&s[MON_OFF_OT_ID]);

    // EXP: 3 bytes big-endian
    m.exp = (static_cast<uint32_t>(s[MON_OFF_EXP + 0]) << 16)
          | (static_cast<uint32_t>(s[MON_OFF_EXP + 1]) <<  8)
          | static_cast<uint32_t>(s[MON_OFF_EXP + 2]);

    // Stat EXP: 5 × u16 LE
    m.stat_exp_hp  = read_u16_le(&s[MON_OFF_HP_EXP]);
    m.stat_exp_atk = read_u16_le(&s[MON_OFF_ATK_EXP]);
    m.stat_exp_def = read_u16_le(&s[MON_OFF_DEF_EXP]);
    m.stat_exp_spd = read_u16_le(&s[MON_OFF_SPD_EXP]);
    m.stat_exp_spc = read_u16_le(&s[MON_OFF_SPC_EXP]);

    // DVs: 2-byte LE; byte0 = ATK(7-4)|DEF(3-0), byte1 = SPD(7-4)|SPC(3-0)
    {
        uint8_t dvb0 = s[MON_OFF_DVS + 0];
        uint8_t dvb1 = s[MON_OFF_DVS + 1];
        m.dvs.atk = (dvb0 >> 4) & 0x0F;
        m.dvs.def =  dvb0       & 0x0F;
        m.dvs.spd = (dvb1 >> 4) & 0x0F;
        m.dvs.spc =  dvb1       & 0x0F;
    }

    m.happiness = s[MON_OFF_HAPPINESS];
    m.pokerus   = s[MON_OFF_POKERUS];

    // Caught data (2 packed bytes)
    {
        uint8_t c0 = s[MON_OFF_CAUGHT_TIME_LVL];
        uint8_t c1 = s[MON_OFF_CAUGHT_GND_LOC];
        m.caught.time_of_day  = static_cast<uint8_t>((c0 & CAUGHT_TIME_MASK) >> 6);
        m.caught.caught_level = static_cast<uint8_t>(c0 & CAUGHT_LEVEL_MASK);
        m.caught.caught_by_boy = (c1 & CAUGHT_GENDER_MASK) != 0;
        m.caught.location      = static_cast<uint8_t>(c1 & CAUGHT_LOCATION_MASK);
    }

    m.level = s[MON_OFF_LEVEL];
    if (m.level < 2 || m.level > 100) {
        // Level 1 is technically MIN_LEVEL=2, but eggs sometimes have level 0.
        // Accept 0 (egg) or 2-100; reject anything else.
        if (m.level != 0) {
            throw SaveImportError(std::format(
                "decode_party: slot {} level {} out of range [0,100]",
                slot_idx, m.level));
        }
    }

    m.status = s[MON_OFF_STATUS];
    // MON_OFF_UNUSED at +33 is skipped

    // Stats: big-endian u16 each
    m.current_hp = read_u16_be(&s[MON_OFF_HP]);
    m.max_hp     = read_u16_be(&s[MON_OFF_MAXHP]);
    m.stat_atk   = read_u16_be(&s[MON_OFF_ATK]);
    m.stat_def   = read_u16_be(&s[MON_OFF_DEF]);
    m.stat_spd   = read_u16_be(&s[MON_OFF_SPD]);
    m.stat_sat   = read_u16_be(&s[MON_OFF_SAT]);
    m.stat_sdf   = read_u16_be(&s[MON_OFF_SDF]);

    // Names (Crystal charmap → UTF-8)
    m.ot_name  = decode_crystal_string_from(ot_name,  OT_NAME_SIZE);
    m.nickname = decode_crystal_string_from(nickname, NICKNAME_SIZE);

    return m;
}

// Encode one party_struct (48 bytes) into base pointer.
static void encode_one_mon(
    const CrystalPartyMon& m,
    uint8_t* s,         // party_struct base (48 bytes)
    uint8_t* ot_buf,    // OT name buffer (11 bytes)
    uint8_t* nick_buf,  // nickname buffer (11 bytes)
    uint8_t slot_idx,
    const PartyCodecDomain& domain)
{
    // Representability: species
    if (m.species == 0) {
        throw SaveExportError(std::format(
            "encode_party: slot {} species is 0 (cannot encode empty species)", slot_idx));
    }
    if (m.species != 254 && static_cast<uint16_t>(m.species) > domain.num_pokemon) {
        throw SaveExportError(std::format(
            "encode_party: slot {} species 0x{:02X} exceeds profile num_pokemon {}",
            slot_idx, m.species, domain.num_pokemon));
    }
    if (m.item != 0 && static_cast<uint16_t>(m.item) >= domain.num_items) {
        throw SaveExportError(std::format(
            "encode_party: slot {} item 0x{:02X} exceeds profile num_items {}",
            slot_idx, m.item, domain.num_items));
    }
    // Level range
    if (m.level > 100) {
        throw SaveExportError(std::format(
            "encode_party: slot {} level {} exceeds MAX_LEVEL 100", slot_idx, m.level));
    }
    // EXP max: Crystal uses 3 bytes = max 16 777 215
    if (m.exp > 0x00FFFFFF) {
        throw SaveExportError(std::format(
            "encode_party: slot {} exp {} exceeds 3-byte maximum", slot_idx, m.exp));
    }

    s[MON_OFF_SPECIES] = m.species;
    s[MON_OFF_ITEM]    = m.item;

    for (uint32_t i = 0; i < NUM_MOVES; ++i) {
        uint8_t move_id = m.moves[i];
        if (move_id != 0 && static_cast<uint16_t>(move_id) > domain.num_moves) {
            throw SaveExportError(std::format(
                "encode_party: slot {} move[{}] 0x{:02X} exceeds profile num_moves {}",
                slot_idx, i, move_id, domain.num_moves));
        }
        s[MON_OFF_MOVES + i] = move_id;
        // PP: pack pp_ups (2 bits) | pp (6 bits)
        uint8_t ppu = m.pp_ups[i] & 0x03;
        uint8_t pp  = m.pp[i]     & PP_MASK;
        s[MON_OFF_PP + i] = static_cast<uint8_t>((ppu << 6) | pp);
    }

    write_u16_le(&s[MON_OFF_OT_ID], m.ot_id);

    // EXP big-endian 3 bytes
    s[MON_OFF_EXP + 0] = static_cast<uint8_t>((m.exp >> 16) & 0xFF);
    s[MON_OFF_EXP + 1] = static_cast<uint8_t>((m.exp >>  8) & 0xFF);
    s[MON_OFF_EXP + 2] = static_cast<uint8_t>( m.exp        & 0xFF);

    // Stat EXP LE
    write_u16_le(&s[MON_OFF_HP_EXP],  m.stat_exp_hp);
    write_u16_le(&s[MON_OFF_ATK_EXP], m.stat_exp_atk);
    write_u16_le(&s[MON_OFF_DEF_EXP], m.stat_exp_def);
    write_u16_le(&s[MON_OFF_SPD_EXP], m.stat_exp_spd);
    write_u16_le(&s[MON_OFF_SPC_EXP], m.stat_exp_spc);

    // DVs LE
    if (m.dvs.atk > 15 || m.dvs.def > 15 || m.dvs.spd > 15 || m.dvs.spc > 15) {
        throw SaveExportError(std::format(
            "encode_party: slot {} DV out of 0-15 range", slot_idx));
    }
    s[MON_OFF_DVS + 0] = static_cast<uint8_t>((m.dvs.atk << 4) | m.dvs.def);
    s[MON_OFF_DVS + 1] = static_cast<uint8_t>((m.dvs.spd << 4) | m.dvs.spc);

    s[MON_OFF_HAPPINESS] = m.happiness;
    s[MON_OFF_POKERUS]   = m.pokerus;

    // Caught data
    {
        uint8_t time = m.caught.time_of_day & 0x03;
        uint8_t lvl  = m.caught.caught_level & 0x3F;
        s[MON_OFF_CAUGHT_TIME_LVL] = static_cast<uint8_t>((time << 6) | lvl);
        uint8_t gender_bit = m.caught.caught_by_boy ? 0x80u : 0x00u;
        uint8_t loc = m.caught.location & 0x7F;
        s[MON_OFF_CAUGHT_GND_LOC]  = static_cast<uint8_t>(gender_bit | loc);
    }

    s[MON_OFF_LEVEL]  = m.level;
    s[MON_OFF_STATUS] = m.status;
    s[MON_OFF_UNUSED] = 0;  // rb_skip padding — always zero

    // Stats big-endian
    write_u16_be(&s[MON_OFF_HP],    m.current_hp);
    write_u16_be(&s[MON_OFF_MAXHP], m.max_hp);
    write_u16_be(&s[MON_OFF_ATK],   m.stat_atk);
    write_u16_be(&s[MON_OFF_DEF],   m.stat_def);
    write_u16_be(&s[MON_OFF_SPD],   m.stat_spd);
    write_u16_be(&s[MON_OFF_SAT],   m.stat_sat);
    write_u16_be(&s[MON_OFF_SDF],   m.stat_sdf);

    // Names
    encode_crystal_string_to(m.ot_name,  ot_buf,   OT_NAME_SIZE);
    encode_crystal_string_to(m.nickname, nick_buf, NICKNAME_SIZE);
}

// ─── Public API ──────────────────────────────────────────────────────────────

CrystalParty decode_party(
    const uint8_t* data,
    uint32_t adj,
    const PartyCodecDomain& domain)
{
    CrystalParty party;

    // wPartyCount
    uint8_t count = data[PARTY_COUNT - adj];
    if (count > PARTY_LENGTH) {
        throw SaveImportError(std::format(
            "decode_party: party_count {} exceeds PARTY_LENGTH {}",
            count, PARTY_LENGTH));
    }
    party.party_count = count;

    if (count == 0) return party;

    // Validate species list terminator
    // Species list: count entries + 0xFF terminator at position count
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t sp = data[PARTY_SPECIES - adj + i];
        if (sp == 0) {
            throw SaveImportError(std::format(
                "decode_party: species_list[{}] = 0 but party_count = {}", i, count));
        }
        if (sp != 254 && static_cast<uint16_t>(sp) > domain.num_pokemon) {
            throw SaveImportError(std::format(
                "decode_party: species_list[{}] = {} exceeds num_pokemon {}",
                i, sp, domain.num_pokemon));
        }
    }
    {
        uint8_t term = data[PARTY_SPECIES - adj + count];
        if (term != 0xFF) {
            throw SaveImportError(std::format(
                "decode_party: expected 0xFF species-list terminator at position {}, got 0x{:02X}",
                count, term));
        }
    }

    // Decode each mon
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t* mon_base   = &data[PARTY_MON_1  - adj + i * PARTYMON_STRUCT_LENGTH];
        const uint8_t* ot_base    = &data[PARTY_OT_NAMES   - adj + i * OT_NAME_SIZE];
        const uint8_t* nick_base  = &data[PARTY_NICKNAMES  - adj + i * NICKNAME_SIZE];

        // Cross-check: species in party_struct[0] must match species list
        uint8_t struct_species = mon_base[MON_OFF_SPECIES];
        uint8_t list_species   = data[PARTY_SPECIES - adj + i];
        if (struct_species != list_species) {
            throw SaveImportError(std::format(
                "decode_party: slot {} struct.species={} != species_list[{}]={}",
                i, struct_species, i, list_species));
        }

        party.party_mons[i] = decode_one_mon(mon_base, ot_base, nick_base, i, domain);
    }

    return party;
}

void encode_party(
    const CrystalParty& party,
    uint8_t* data,
    const PartyCodecDomain& domain)
{
    if (party.party_count > PARTY_LENGTH) {
        throw SaveExportError(std::format(
            "encode_party: party_count {} exceeds PARTY_LENGTH {}",
            party.party_count, PARTY_LENGTH));
    }

    uint8_t count = party.party_count;

    // Write party count byte
    data[PARTY_COUNT] = count;

    // Write species list + terminator
    for (uint8_t i = 0; i < count; ++i) {
        data[PARTY_SPECIES + i] = party.party_mons[i].species;
    }
    data[PARTY_SPECIES + count] = 0xFF;  // terminator
    // Zero remaining species slots (after terminator, up to PARTY_LENGTH)
    for (uint8_t i = count + 1; i <= PARTY_LENGTH; ++i) {
        data[PARTY_SPECIES + i] = 0;
    }

    // Encode each mon struct + names
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t* mon_base  = &data[PARTY_MON_1    + i * PARTYMON_STRUCT_LENGTH];
        uint8_t* ot_base   = &data[PARTY_OT_NAMES  + i * OT_NAME_SIZE];
        uint8_t* nick_base = &data[PARTY_NICKNAMES + i * NICKNAME_SIZE];
        encode_one_mon(party.party_mons[i], mon_base, ot_base, nick_base, i, domain);
    }

    // Zero out unused party slots in the struct region and name buffers
    for (uint8_t i = count; i < PARTY_LENGTH; ++i) {
        uint8_t* mon_base  = &data[PARTY_MON_1    + i * PARTYMON_STRUCT_LENGTH];
        uint8_t* ot_base   = &data[PARTY_OT_NAMES  + i * OT_NAME_SIZE];
        uint8_t* nick_base = &data[PARTY_NICKNAMES + i * NICKNAME_SIZE];
        std::fill(mon_base,  mon_base  + PARTYMON_STRUCT_LENGTH, 0);
        std::fill(ot_base,   ot_base   + OT_NAME_SIZE,           0x50);  // 0x50 = '@' terminator
        std::fill(nick_base, nick_base + NICKNAME_SIZE,           0x50);
    }
}

}  // namespace crystal
