#pragma once
// crystal/script/crystal_command.hpp
// Typed lossless CrystalCommand IR for all Crystal opcodes 0x00-0xA9
//
// STAGE 1 REQUIREMENTS:
// - Every valid Crystal command has a typed variant
// - Every decoded operand is preserved
// - Source span with exact bytes is preserved
// - Round-trip encoding must match original bytes exactly
// - Zero Op_Raw for known opcodes

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <span>
#include "engine/core/types.hpp"  // For MovementCommand

namespace crystal {

// Source span preserves exact bytes for round-trip validation
struct SourceSpan {
    uint32_t rom_address;       // Flat ROM address of first byte
    std::vector<uint8_t> raw_bytes;  // Exact bytes including opcode
    
    uint8_t opcode() const { return raw_bytes.empty() ? 0 : raw_bytes[0]; }
    size_t size() const { return raw_bytes.size(); }
};

// Decode status for each command
enum class DecodeStatus : uint8_t {
    Success,            // Fully decoded with valid operands
    Unlowered,          // Decoded but not yet semantically lowered (all start here)
    Malformed,          // Decode failed (invalid operand, truncated, etc.)
};

// Forward declaration of LabelRef for control flow targets
struct CrystalLabelRef {
    uint32_t rom_address;       // Flat target address
    std::string symbol_name;    // Optional symbol name if known
};

// Map ID is encoded as (group, map) pair
struct CrystalMapId {
    uint8_t group;
    uint8_t map;
    
    uint16_t as_u16() const { return (static_cast<uint16_t>(group) << 8) | map; }
};

// =============================================================================
// CONTROL FLOW COMMANDS (0x00-0x0D)
// =============================================================================

// 0x00: scall - local subroutine call (2-byte pointer)
struct Cmd_Scall {
    uint16_t pointer;           // Local pointer within bank
    CrystalLabelRef target;     // Resolved target
};

// 0x01: farscall - far subroutine call (3-byte bank:pointer)
struct Cmd_Farscall {
    uint8_t bank;
    uint16_t pointer;
    CrystalLabelRef target;
};

// 0x02: memcall - call via memory address (2-byte RAM address)
struct Cmd_Memcall {
    uint16_t ram_address;
};

// 0x03: sjump - local unconditional jump (2-byte pointer)
struct Cmd_Sjump {
    uint16_t pointer;
    CrystalLabelRef target;
};

// 0x04: farsjump - far unconditional jump (3-byte bank:pointer)
struct Cmd_Farsjump {
    uint8_t bank;
    uint16_t pointer;
    CrystalLabelRef target;
};

// 0x05: memjump - jump via memory address (2-byte RAM address)
struct Cmd_Memjump {
    uint16_t ram_address;
};

// 0x06: ifequal - branch if wScriptVar == value
struct Cmd_Ifequal {
    uint8_t value;
    uint16_t pointer;
    CrystalLabelRef target;
};

// 0x07: ifnotequal - branch if wScriptVar != value
struct Cmd_Ifnotequal {
    uint8_t value;
    uint16_t pointer;
    CrystalLabelRef target;
};

// 0x08: iffalse - branch if wScriptVar is false (0)
struct Cmd_Iffalse {
    uint16_t pointer;
    CrystalLabelRef target;
};

// 0x09: iftrue - branch if wScriptVar is true (nonzero)
struct Cmd_Iftrue {
    uint16_t pointer;
    CrystalLabelRef target;
};

// 0x0A: ifgreater - branch if wScriptVar > value
struct Cmd_Ifgreater {
    uint8_t value;
    uint16_t pointer;
    CrystalLabelRef target;
};

// 0x0B: ifless - branch if wScriptVar < value
struct Cmd_Ifless {
    uint8_t value;
    uint16_t pointer;
    CrystalLabelRef target;
};

// 0x0C: jumpstd - jump to standard script
struct Cmd_Jumpstd {
    uint16_t std_id;
};

// 0x0D: callstd - call standard script
struct Cmd_Callstd {
    uint16_t std_id;
};

// =============================================================================
// ASM/SPECIAL COMMANDS (0x0E-0x10)
// =============================================================================

// 0x0E: callasm - call native ASM routine (3-byte bank:pointer)
struct Cmd_Callasm {
    uint8_t bank;
    uint16_t pointer;
    uint32_t flat_address;      // Resolved flat address
};

// 0x0F: special - call special function by ID
struct Cmd_Special {
    uint16_t special_id;
};

// 0x10: memcallasm - call ASM via memory address (2-byte RAM address)
struct Cmd_Memcallasm {
    uint16_t ram_address;
};

// =============================================================================
// MAP SCENE COMMANDS (0x11-0x14)
// =============================================================================

// 0x11: checkmapscene - check scene value for a map
struct Cmd_Checkmapscene {
    CrystalMapId map;
};

// 0x12: setmapscene - set scene value for a map
struct Cmd_Setmapscene {
    CrystalMapId map;
    uint8_t scene;
};

// 0x13: checkscene - check current map's scene value
struct Cmd_Checkscene {};

// 0x14: setscene - set current map's scene value
struct Cmd_Setscene {
    uint8_t scene;
};

// =============================================================================
// VARIABLE COMMANDS (0x15-0x1E)
// =============================================================================

// 0x15: setval - set wScriptVar to immediate value
struct Cmd_Setval {
    uint8_t value;
};

// 0x16: addval - add immediate value to wScriptVar
struct Cmd_Addval {
    uint8_t value;
};

// 0x17: random - set wScriptVar to random(0..value-1)
struct Cmd_Random {
    uint8_t range;
};

// 0x18: checkver - check game version (no params)
struct Cmd_Checkver {};

// 0x19: readmem - read byte from RAM address into wScriptVar
struct Cmd_Readmem {
    uint16_t ram_address;
};

// 0x1A: writemem - write wScriptVar to RAM address
struct Cmd_Writemem {
    uint16_t ram_address;
};

// 0x1B: loadmem - write immediate value to RAM address
struct Cmd_Loadmem {
    uint16_t ram_address;
    uint8_t value;
};

// 0x1C: readvar - read from script variable table
struct Cmd_Readvar {
    uint8_t var_id;
};

// 0x1D: writevar - write wScriptVar to variable table
struct Cmd_Writevar {
    uint8_t var_id;
};

// 0x1E: loadvar - write immediate to variable table
struct Cmd_Loadvar {
    uint8_t var_id;
    uint8_t value;
};

// =============================================================================
// ITEM COMMANDS (0x1F-0x27)
// =============================================================================

// 0x1F: giveitem - give item to player
struct Cmd_Giveitem {
    uint8_t item;
    uint8_t quantity;
};

// 0x20: takeitem - take item from player
struct Cmd_Takeitem {
    uint8_t item;
    uint8_t quantity;
};

// 0x21: checkitem - check if player has item
struct Cmd_Checkitem {
    uint8_t item;
};

// 0x22: givemoney - give money (account + 3-byte BCD amount)
struct Cmd_Givemoney {
    uint8_t account;
    uint8_t money_byte1;    // High byte
    uint8_t money_byte2;    // Mid byte  
    uint8_t money_byte3;    // Low byte
    
    uint32_t amount() const {
        return (static_cast<uint32_t>(money_byte1) << 16) |
               (static_cast<uint32_t>(money_byte2) << 8) |
               static_cast<uint32_t>(money_byte3);
    }
};

// 0x23: takemoney - take money (account + 3-byte BCD amount)
struct Cmd_Takemoney {
    uint8_t account;
    uint8_t money_byte1;
    uint8_t money_byte2;
    uint8_t money_byte3;
    
    uint32_t amount() const {
        return (static_cast<uint32_t>(money_byte1) << 16) |
               (static_cast<uint32_t>(money_byte2) << 8) |
               static_cast<uint32_t>(money_byte3);
    }
};

// 0x24: checkmoney - check if player has money (account + 3-byte BCD)
struct Cmd_Checkmoney {
    uint8_t account;
    uint8_t money_byte1;
    uint8_t money_byte2;
    uint8_t money_byte3;
    
    uint32_t amount() const {
        return (static_cast<uint32_t>(money_byte1) << 16) |
               (static_cast<uint32_t>(money_byte2) << 8) |
               static_cast<uint32_t>(money_byte3);
    }
};

// 0x25: givecoins - give coins
struct Cmd_Givecoins {
    uint16_t coins;
};

// 0x26: takecoins - take coins
struct Cmd_Takecoins {
    uint16_t coins;
};

// 0x27: checkcoins - check if player has coins
struct Cmd_Checkcoins {
    uint16_t coins;
};

// =============================================================================
// PHONE COMMANDS (0x28-0x2A)
// =============================================================================

// 0x28: addcellnum - add phone number
struct Cmd_Addcellnum {
    uint8_t person;
};

// 0x29: delcellnum - delete phone number
struct Cmd_Delcellnum {
    uint8_t person;
};

// 0x2A: checkcellnum - check if phone number exists
struct Cmd_Checkcellnum {
    uint8_t person;
};

// =============================================================================
// TIME/POKEMON COMMANDS (0x2B-0x30)
// =============================================================================

// 0x2B: checktime - check time of day
struct Cmd_Checktime {
    uint8_t time;
};

// 0x2C: checkpoke - check if party has pokemon species
struct Cmd_Checkpoke {
    uint8_t pokemon;
};

// 0x2D: givepoke - give pokemon to player
// Complex: has optional trainer byte that controls additional data
struct Cmd_Givepoke {
    uint8_t pokemon;
    uint8_t level;
    uint8_t item;
    uint8_t trainer;        // If nonzero, has nickname/OT pointers
    uint16_t nickname_ptr;  // Only if trainer != 0
    uint16_t ot_name_ptr;   // Only if trainer != 0
    bool has_extra_data;    // True if trainer != 0
};

// 0x2E: giveegg - give pokemon egg
struct Cmd_Giveegg {
    uint8_t pokemon;
    uint8_t level;
};

// 0x2F: givepokemail - give pokemon mail
// Pointer is local to script bank - resolve at decode time
struct Cmd_Givepokemail {
    uint16_t pointer;           // Raw 16-bit pointer for round-trip encoding
    uint32_t flat_address;      // Resolved flat ROM address for registry extraction
};

// 0x30: checkpokemail - check pokemon mail
// Pointer is local to script bank - resolve at decode time
struct Cmd_Checkpokemail {
    uint16_t pointer;           // Raw 16-bit pointer for round-trip encoding
    uint32_t flat_address;      // Resolved flat ROM address for registry extraction
};

// =============================================================================
// EVENT/FLAG COMMANDS (0x31-0x36)
// =============================================================================

// 0x31: checkevent - check event flag
struct Cmd_Checkevent {
    uint16_t event_flag;
};

// 0x32: clearevent - clear event flag
struct Cmd_Clearevent {
    uint16_t event_flag;
};

// 0x33: setevent - set event flag
struct Cmd_Setevent {
    uint16_t event_flag;
};

// 0x34: checkflag - check engine flag
struct Cmd_Checkflag {
    uint16_t engine_flag;
};

// 0x35: clearflag - clear engine flag
struct Cmd_Clearflag {
    uint16_t engine_flag;
};

// 0x36: setflag - set engine flag
struct Cmd_Setflag {
    uint16_t engine_flag;
};

// =============================================================================
// WILD ENCOUNTER COMMANDS (0x37-0x38)
// =============================================================================

// 0x37: wildon - enable wild encounters
struct Cmd_Wildon {};

// 0x38: wildoff - disable wild encounters
struct Cmd_Wildoff {};

// =============================================================================
// MAP/WARP COMMANDS (0x39-0x3C)
// =============================================================================

// 0x39: xycompare - compare XY coordinates (2-byte pointer to coord table)
struct Cmd_Xycompare {
    uint16_t pointer;
};

// 0x3A: warpmod - modify warp destination
struct Cmd_Warpmod {
    uint8_t warp_id;
    CrystalMapId map;
};

// 0x3B: blackoutmod - set blackout respawn map
struct Cmd_Blackoutmod {
    CrystalMapId map;
};

// 0x3C: warp - warp to location
struct Cmd_Warp {
    CrystalMapId map;
    uint8_t x;
    uint8_t y;
};

// =============================================================================
// STRING FORMATTING COMMANDS (0x3D-0x44)
// =============================================================================

// 0x3D: getmoney - format money to string buffer
struct Cmd_Getmoney {
    uint8_t strbuf;
    uint8_t account;
};

// 0x3E: getcoins - format coins to string buffer
struct Cmd_Getcoins {
    uint8_t strbuf;
};

// 0x3F: getnum - format number to string buffer
struct Cmd_Getnum {
    uint8_t strbuf;
};

// 0x40: getmonname - get pokemon name to string buffer
struct Cmd_Getmonname {
    uint8_t strbuf;
    uint8_t pokemon;
};

// 0x41: getitemname - get item name to string buffer
struct Cmd_Getitemname {
    uint8_t strbuf;
    uint8_t item;
};

// 0x42: getcurlandmarkname - get current landmark name
struct Cmd_Getcurlandmarkname {
    uint8_t strbuf;
};

// 0x43: gettrainername - get trainer name to string buffer
struct Cmd_Gettrainername {
    uint8_t strbuf;
    uint8_t trainer_group;
    uint8_t trainer_id;
};

// 0x44: getstring - get string from pointer to buffer
struct Cmd_Getstring {
    uint8_t strbuf;
    uint16_t text_pointer;
};

// =============================================================================
// ITEM NOTIFY COMMANDS (0x45-0x46)
// =============================================================================

// 0x45: itemnotify - notify item received
struct Cmd_Itemnotify {};

// 0x46: pocketisfull - check if pocket is full
struct Cmd_Pocketisfull {};

// =============================================================================
// TEXT COMMANDS (0x47-0x55)
// =============================================================================

// 0x47: opentext - open text box
struct Cmd_Opentext {};

// 0x48: reanchormap - re-anchor map (optional dummy byte)
struct Cmd_Reanchormap {
    uint8_t dummy;      // Usually 0 or present as padding
};

// 0x49: closetext - close text box
struct Cmd_Closetext {};

// 0x4A: writeunusedbyte - write unused byte (debugging?)
struct Cmd_Writeunusedbyte {
    uint8_t byte;
};

// 0x4B: farwritetext - write text from far pointer
struct Cmd_Farwritetext {
    uint8_t bank;
    uint16_t pointer;
};

// 0x4C: writetext - write text from local pointer
struct Cmd_Writetext {
    uint16_t text_pointer;
};

// 0x4D: repeattext - repeat text
struct Cmd_Repeattext {
    uint8_t byte1;
    uint8_t byte2;
};

// 0x4E: yesorno - show yes/no dialog
struct Cmd_Yesorno {};

// 0x4F: loadmenu - load menu
struct Cmd_Loadmenu {
    uint16_t menu_header;
};

// 0x50: closewindow - close window
struct Cmd_Closewindow {};

// 0x51: jumptextfaceplayer - face player and jump to text
struct Cmd_Jumptextfaceplayer {
    uint16_t text_pointer;
};

// 0x52: farjumptext - jump to text from far pointer
struct Cmd_Farjumptext {
    uint8_t bank;
    uint16_t pointer;
};

// 0x53: jumptext - jump to text and end
struct Cmd_Jumptext {
    uint16_t text_pointer;
};

// 0x54: waitbutton - wait for button press
struct Cmd_Waitbutton {};

// 0x55: promptbutton - prompt button (similar to waitbutton)
struct Cmd_Promptbutton {};

// =============================================================================
// POKEMON DISPLAY COMMANDS (0x56-0x5B)
// =============================================================================

// 0x56: pokepic - show pokemon picture
struct Cmd_Pokepic {
    uint8_t pokemon;
};

// 0x57: closepokepic - close pokemon picture
struct Cmd_Closepokepic {};

// 0x58: _2dmenu - 2D menu
struct Cmd_2dmenu {};

// 0x59: verticalmenu - vertical menu
struct Cmd_Verticalmenu {};

// 0x5A: loadpikachudata - load pikachu data (Crystal remnant)
struct Cmd_Loadpikachudata {};

// 0x5B: randomwildmon - generate random wild pokemon
struct Cmd_Randomwildmon {};

// =============================================================================
// BATTLE SETUP COMMANDS (0x5C-0x67)
// =============================================================================

// 0x5C: loadtemptrainer - load temporary trainer data
struct Cmd_Loadtemptrainer {};

// 0x5D: loadwildmon - load wild pokemon for battle
struct Cmd_Loadwildmon {
    uint8_t pokemon;
    uint8_t level;
};

// 0x5E: loadtrainer - load trainer for battle
struct Cmd_Loadtrainer {
    uint8_t trainer_group;
    uint8_t trainer_id;
};

// 0x5F: startbattle - start the loaded battle
struct Cmd_Startbattle {};

// 0x60: reloadmapafterbattle - reload map after battle
struct Cmd_Reloadmapafterbattle {};

// 0x61: catchtutorial - catch tutorial
struct Cmd_Catchtutorial {
    uint8_t byte;
};

// 0x62: trainertext - set trainer text index
struct Cmd_Trainertext {
    uint8_t text_id;
};

// 0x63: trainerflagaction - trainer flag action
struct Cmd_Trainerflagaction {
    uint8_t action;
};

// 0x64: winlosstext - set win/loss text pointers
// Both pointers are local to script bank - resolve at decode time
struct Cmd_Winlosstext {
    uint16_t win_text;              // Raw 16-bit pointer for round-trip encoding
    uint16_t loss_text;             // Raw 16-bit pointer for round-trip encoding
    uint32_t win_text_address;      // Resolved flat ROM address (0 = no text)
    uint32_t loss_text_address;     // Resolved flat ROM address (0 = no text)
};

// 0x65: scripttalkafter - script talk after battle
struct Cmd_Scripttalkafter {};

// 0x66: endifjustbattled - end if just battled
struct Cmd_Endifjustbattled {};

// 0x67: checkjustbattled - check if just battled
struct Cmd_Checkjustbattled {};

// =============================================================================
// MOVEMENT COMMANDS (0x68-0x77)
// =============================================================================

// 0x68: setlasttalked - set last talked object
struct Cmd_Setlasttalked {
    uint8_t object_id;
};

// 0x69: applymovement - apply movement to object
struct Cmd_Applymovement {
    uint8_t object_id;
    uint16_t movement_pointer;          // ROM pointer (frontend only)
    std::vector<uint8_t> raw_movements; // Raw bytes for round-trip validation
    std::vector<enginemon::MovementCommand> commands;  // Parsed semantic commands
};

// 0x6A: applymovementlasttalked - apply movement to last talked
struct Cmd_Applymovementlasttalked {
    uint16_t movement_pointer;          // ROM pointer (frontend only)
    std::vector<uint8_t> raw_movements; // Raw bytes for round-trip validation
    std::vector<enginemon::MovementCommand> commands;  // Parsed semantic commands
};

// 0x6B: faceplayer - face the player
struct Cmd_Faceplayer {};

// 0x6C: faceobject - face another object
struct Cmd_Faceobject {
    uint8_t object1;
    uint8_t object2;
};

// 0x6D: variablesprite - set variable sprite
struct Cmd_Variablesprite {
    uint8_t slot;
    uint8_t sprite;
};

// 0x6E: disappear - hide object
struct Cmd_Disappear {
    uint8_t object_id;
};

// 0x6F: appear - show object
struct Cmd_Appear {
    uint8_t object_id;
};

// 0x70: follow - make object follow another
struct Cmd_Follow {
    uint8_t object2;
    uint8_t object1;
};

// 0x71: stopfollow - stop following
struct Cmd_Stopfollow {};

// 0x72: moveobject - teleport object to position
struct Cmd_Moveobject {
    uint8_t object_id;
    uint8_t x;
    uint8_t y;
};

// 0x73: writeobjectxy - write object XY to memory
struct Cmd_Writeobjectxy {
    uint8_t object_id;
};

// 0x74: loademote - load emote sprite
struct Cmd_Loademote {
    uint8_t bubble;
};

// 0x75: showemote - show emote above object
struct Cmd_Showemote {
    uint8_t bubble;
    uint8_t object_id;
    uint8_t time;
};

// 0x76: turnobject - turn object to face direction
struct Cmd_Turnobject {
    uint8_t object_id;
    uint8_t facing;
};

// 0x77: follownotexact - follow not exact
struct Cmd_Follownotexact {
    uint8_t object2;
    uint8_t object1;
};

// =============================================================================
// EFFECT COMMANDS (0x78-0x7E)
// =============================================================================

// 0x78: earthquake - screen shake effect
struct Cmd_Earthquake {
    uint8_t param;
};

// 0x79: changemapblocks - change map blocks (3-byte bank:pointer)
struct Cmd_Changemapblocks {
    uint8_t bank;
    uint16_t pointer;
};

// 0x7A: changeblock - change single map block
struct Cmd_Changeblock {
    uint8_t x;
    uint8_t y;
    uint8_t block;
};

// 0x7B: reloadmap - reload current map
struct Cmd_Reloadmap {};

// 0x7C: refreshmap - refresh map display
struct Cmd_Refreshmap {};

// 0x7D: writecmdqueue - write to command queue
struct Cmd_Writecmdqueue {
    uint16_t queue_pointer;
};

// 0x7E: delcmdqueue - delete from command queue
struct Cmd_Delcmdqueue {
    uint8_t byte;
};

// =============================================================================
// AUDIO COMMANDS (0x7F-0x88)
// =============================================================================

// 0x7F: playmusic - play music
struct Cmd_Playmusic {
    uint16_t music;
};

// 0x80: encountermusic - play encounter music
struct Cmd_Encountermusic {};

// 0x81: musicfadeout - fade out music
struct Cmd_Musicfadeout {
    uint16_t music;
    uint8_t fadetime;
};

// 0x82: playmapmusic - play map's music
struct Cmd_Playmapmusic {};

// 0x83: dontrestartmapmusic - don't restart map music
struct Cmd_Dontrestartmapmusic {};

// 0x84: cry - play pokemon cry
struct Cmd_Cry {
    uint16_t cry_id;
};

// 0x85: playsound - play sound effect
struct Cmd_Playsound {
    uint16_t sound;
};

// 0x86: waitsfx - wait for sound effect to finish
struct Cmd_Waitsfx {};

// 0x87: warpsound - play warp sound
struct Cmd_Warpsound {};

// 0x88: specialsound - play special sound
struct Cmd_Specialsound {};

// =============================================================================
// MISC CONTROL COMMANDS (0x89-0x93)
// =============================================================================

// 0x89: autoinput - automated input (3-byte bank:pointer)
struct Cmd_Autoinput {
    uint8_t bank;
    uint16_t pointer;
};

// 0x8A: newloadmap - load map with method
struct Cmd_Newloadmap {
    uint8_t method;
};

// 0x8B: pause - pause for frames
struct Cmd_Pause {
    uint8_t length;
};

// 0x8C: deactivatefacing - deactivate facing
struct Cmd_Deactivatefacing {
    uint8_t time;
};

// 0x8D: sdefer - defer script (local pointer)
struct Cmd_Sdefer {
    uint16_t pointer;
};

// 0x8E: warpcheck - check warp conditions
struct Cmd_Warpcheck {};

// 0x8F: stopandsjump - stop and jump to script
struct Cmd_Stopandsjump {
    uint16_t pointer;
};

// 0x90: endcallback - end callback/return
struct Cmd_Endcallback {};

// 0x91: end - end script (THE main end command)
struct Cmd_End {};

// 0x92: reloadend - reload and end
struct Cmd_Reloadend {
    uint8_t method;
};

// 0x93: endall - end all scripts
struct Cmd_Endall {};

// =============================================================================
// COMMERCE COMMANDS (0x94-0x9D)
// =============================================================================

// 0x94: pokemart - open pokemart
struct Cmd_Pokemart {
    uint8_t dialog_id;
    uint16_t mart_id;
};

// 0x95: elevator - elevator menu
struct Cmd_Elevator {
    uint16_t floor_list;          // Raw pointer (local to script bank) for round-trip encoding
    uint32_t floor_list_address;  // Flat ROM address (resolved from bank:pointer) for registry lookup
};

// 0x96: trade - in-game trade
struct Cmd_Trade {
    uint8_t trade_id;
};

// 0x97: askforphonenumber - ask for phone number
struct Cmd_Askforphonenumber {
    uint8_t number;
};

// 0x98: phonecall - phone call
struct Cmd_Phonecall {
    uint16_t caller_name;
};

// 0x99: hangup - hang up phone
struct Cmd_Hangup {};

// 0x9A: describedecoration - describe decoration
struct Cmd_Describedecoration {
    uint8_t byte;
};

// 0x9B: fruittree - fruit tree interaction
struct Cmd_Fruittree {
    uint8_t tree_id;
};

// 0x9C: specialphonecall - special phone call
struct Cmd_Specialphonecall {
    uint16_t call_id;
};

// 0x9D: checkphonecall - check phone call status
struct Cmd_Checkphonecall {};

// =============================================================================
// VERBOSE ITEM COMMANDS (0x9E-0x9F)
// =============================================================================

// 0x9E: verbosegiveitem - verbose give item (with message)
struct Cmd_Verbosegiveitem {
    uint8_t item;
    uint8_t quantity;
};

// 0x9F: verbosegiveitemvar - verbose give item from variable
struct Cmd_Verbosegiveitemvar {
    uint8_t item;
    uint8_t var;
};

// =============================================================================
// END GAME COMMANDS (0xA0-0xA9)
// =============================================================================

// 0xA0: swarm - pokemon swarm
struct Cmd_Swarm {
    uint8_t flag;
    CrystalMapId map;
};

// 0xA1: halloffame - hall of fame sequence
struct Cmd_Halloffame {};

// 0xA2: credits - credits sequence
struct Cmd_Credits {};

// 0xA3: warpfacing - warp with facing direction
struct Cmd_Warpfacing {
    uint8_t facing;
    CrystalMapId map;
    uint8_t x;
    uint8_t y;
};

// 0xA4: battletowertext - battle tower text
struct Cmd_Battletowertext {
    uint8_t bttext_id;
};

// 0xA5: getlandmarkname - get landmark name
struct Cmd_Getlandmarkname {
    uint8_t strbuf;
    uint8_t landmark_id;
};

// 0xA6: gettrainerclassname - get trainer class name
struct Cmd_Gettrainerclassname {
    uint8_t strbuf;
    uint8_t trainer_group;
};

// 0xA7: getname - get name by type
struct Cmd_Getname {
    uint8_t strbuf;
    uint8_t type;
    uint8_t id;
};

// 0xA8: wait - wait for duration
struct Cmd_Wait {
    uint8_t duration;
};

// 0xA9: checksave - check save status
struct Cmd_Checksave {};

// =============================================================================
// UNKNOWN COMMAND (for truly unknown opcodes beyond 0xA9)
// =============================================================================

// For opcodes >= 0xAA that are truly unknown/invalid
struct Cmd_Unknown {
    uint8_t opcode;
};

// =============================================================================
// CRYSTAL COMMAND VARIANT
// =============================================================================

using CrystalCommandData = std::variant<
    // Control flow (0x00-0x0D)
    Cmd_Scall, Cmd_Farscall, Cmd_Memcall, Cmd_Sjump, Cmd_Farsjump, Cmd_Memjump,
    Cmd_Ifequal, Cmd_Ifnotequal, Cmd_Iffalse, Cmd_Iftrue, Cmd_Ifgreater, Cmd_Ifless,
    Cmd_Jumpstd, Cmd_Callstd,
    
    // ASM/Special (0x0E-0x10)
    Cmd_Callasm, Cmd_Special, Cmd_Memcallasm,
    
    // Map scene (0x11-0x14)
    Cmd_Checkmapscene, Cmd_Setmapscene, Cmd_Checkscene, Cmd_Setscene,
    
    // Variables (0x15-0x1E)
    Cmd_Setval, Cmd_Addval, Cmd_Random, Cmd_Checkver,
    Cmd_Readmem, Cmd_Writemem, Cmd_Loadmem,
    Cmd_Readvar, Cmd_Writevar, Cmd_Loadvar,
    
    // Items (0x1F-0x27)
    Cmd_Giveitem, Cmd_Takeitem, Cmd_Checkitem,
    Cmd_Givemoney, Cmd_Takemoney, Cmd_Checkmoney,
    Cmd_Givecoins, Cmd_Takecoins, Cmd_Checkcoins,
    
    // Phone (0x28-0x2A)
    Cmd_Addcellnum, Cmd_Delcellnum, Cmd_Checkcellnum,
    
    // Time/Pokemon (0x2B-0x30)
    Cmd_Checktime, Cmd_Checkpoke, Cmd_Givepoke, Cmd_Giveegg,
    Cmd_Givepokemail, Cmd_Checkpokemail,

    // Events/Flags (0x31-0x36)
    Cmd_Checkevent, Cmd_Clearevent, Cmd_Setevent,
    Cmd_Checkflag, Cmd_Clearflag, Cmd_Setflag,
    
    // Wild encounters (0x37-0x38)
    Cmd_Wildon, Cmd_Wildoff,
    
    // Map/Warp (0x39-0x3C)
    Cmd_Xycompare, Cmd_Warpmod, Cmd_Blackoutmod, Cmd_Warp,
    
    // String formatting (0x3D-0x44)
    Cmd_Getmoney, Cmd_Getcoins, Cmd_Getnum,
    Cmd_Getmonname, Cmd_Getitemname, Cmd_Getcurlandmarkname,
    Cmd_Gettrainername, Cmd_Getstring,
    
    // Item notify (0x45-0x46)
    Cmd_Itemnotify, Cmd_Pocketisfull,
    
    // Text (0x47-0x55)
    Cmd_Opentext, Cmd_Reanchormap, Cmd_Closetext, Cmd_Writeunusedbyte,
    Cmd_Farwritetext, Cmd_Writetext, Cmd_Repeattext, Cmd_Yesorno,
    Cmd_Loadmenu, Cmd_Closewindow, Cmd_Jumptextfaceplayer,
    Cmd_Farjumptext, Cmd_Jumptext, Cmd_Waitbutton, Cmd_Promptbutton,
    
    // Pokemon display (0x56-0x5B)
    Cmd_Pokepic, Cmd_Closepokepic, Cmd_2dmenu, Cmd_Verticalmenu,
    Cmd_Loadpikachudata, Cmd_Randomwildmon,
    
    // Battle setup (0x5C-0x67)
    Cmd_Loadtemptrainer, Cmd_Loadwildmon, Cmd_Loadtrainer, Cmd_Startbattle,
    Cmd_Reloadmapafterbattle, Cmd_Catchtutorial, Cmd_Trainertext,
    Cmd_Trainerflagaction, Cmd_Winlosstext, Cmd_Scripttalkafter,
    Cmd_Endifjustbattled, Cmd_Checkjustbattled,

    // Movement (0x68-0x77)
    Cmd_Setlasttalked, Cmd_Applymovement, Cmd_Applymovementlasttalked,
    Cmd_Faceplayer, Cmd_Faceobject, Cmd_Variablesprite,
    Cmd_Disappear, Cmd_Appear, Cmd_Follow, Cmd_Stopfollow,
    Cmd_Moveobject, Cmd_Writeobjectxy, Cmd_Loademote, Cmd_Showemote,
    Cmd_Turnobject, Cmd_Follownotexact,
    
    // Effects (0x78-0x7E)
    Cmd_Earthquake, Cmd_Changemapblocks, Cmd_Changeblock,
    Cmd_Reloadmap, Cmd_Refreshmap, Cmd_Writecmdqueue, Cmd_Delcmdqueue,
    
    // Audio (0x7F-0x88)
    Cmd_Playmusic, Cmd_Encountermusic, Cmd_Musicfadeout,
    Cmd_Playmapmusic, Cmd_Dontrestartmapmusic, Cmd_Cry,
    Cmd_Playsound, Cmd_Waitsfx, Cmd_Warpsound, Cmd_Specialsound,
    
    // Misc control (0x89-0x93)
    Cmd_Autoinput, Cmd_Newloadmap, Cmd_Pause, Cmd_Deactivatefacing,
    Cmd_Sdefer, Cmd_Warpcheck, Cmd_Stopandsjump,
    Cmd_Endcallback, Cmd_End, Cmd_Reloadend, Cmd_Endall,
    
    // Commerce (0x94-0x9D)
    Cmd_Pokemart, Cmd_Elevator, Cmd_Trade, Cmd_Askforphonenumber,
    Cmd_Phonecall, Cmd_Hangup, Cmd_Describedecoration, Cmd_Fruittree,
    Cmd_Specialphonecall, Cmd_Checkphonecall,
    
    // Verbose items (0x9E-0x9F)
    Cmd_Verbosegiveitem, Cmd_Verbosegiveitemvar,
    
    // End game (0xA0-0xA9)
    Cmd_Swarm, Cmd_Halloffame, Cmd_Credits, Cmd_Warpfacing,
    Cmd_Battletowertext, Cmd_Getlandmarkname, Cmd_Gettrainerclassname,
    Cmd_Getname, Cmd_Wait, Cmd_Checksave,
    
    // Unknown (>= 0xAA)
    Cmd_Unknown
>;

// =============================================================================
// CRYSTAL COMMAND (with metadata)
// =============================================================================

struct CrystalCommand {
    CrystalCommandData data;
    SourceSpan span;
    DecodeStatus status = DecodeStatus::Unlowered;
    
    // Helper to get opcode
    uint8_t opcode() const { return span.opcode(); }
    
    // Helper to check if this is a terminal command (ends execution flow)
    bool is_terminator() const;
    
    // Helper to check if this is a branch/jump command
    bool is_branch() const;
    
    // Helper to check if this is a call command
    bool is_call() const;

    // Out-of-line special members: suppresses per-TU instantiation of
    // CrystalCommandData (171-alt) destructor/move/copy machinery.
    // Defined in crystal_command.cpp (the single TU that owns the variant cost).
    CrystalCommand();
    ~CrystalCommand();
    CrystalCommand(const CrystalCommand&);
    CrystalCommand(CrystalCommand&&) noexcept;
    CrystalCommand& operator=(const CrystalCommand&);
    CrystalCommand& operator=(CrystalCommand&&) noexcept;
};

// extern template: suppress vector<CrystalCommand> instantiation in every TU.
extern template class std::vector<CrystalCommand>;

// =============================================================================
// ROUND-TRIP ENCODING
// =============================================================================

// Re-encode a CrystalCommand back to bytes for validation
// Returns empty vector if encoding fails
std::vector<uint8_t> encode_crystal_command(const CrystalCommand& cmd);

// Validate that a decoded command round-trips correctly
bool validate_round_trip(const CrystalCommand& cmd);

// =============================================================================
// CRYSTAL SCRIPT IR (new typed version)
// =============================================================================

struct CrystalScriptIR {
    std::string name;
    uint32_t entry_address = 0;  // Entry point address
    uint32_t rom_start = 0;
    uint32_t rom_end = 0;
    
    std::vector<CrystalCommand> commands;
    
    // Statistics
    size_t unknown_count() const;
    bool is_fully_decoded() const { return unknown_count() == 0; }

    // Out-of-line special members (see CrystalCommand for rationale).
    CrystalScriptIR();
    ~CrystalScriptIR();
    CrystalScriptIR(const CrystalScriptIR&);
    CrystalScriptIR(CrystalScriptIR&&) noexcept;
    CrystalScriptIR& operator=(const CrystalScriptIR&);
    CrystalScriptIR& operator=(CrystalScriptIR&&) noexcept;
};

} // namespace crystal
