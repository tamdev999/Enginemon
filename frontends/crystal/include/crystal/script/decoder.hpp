#pragma once
// crystal/script/decoder.hpp
// Decodes Crystal ROM script bytecode into IR
// Uses pokecrystal's documented opcode structure (macros/scripts/events.asm)

#include "crystal/script/ir.hpp"
#include "crystal/rom/loader.hpp"
#include "crystal/rom/symbol_map.hpp"
#include <span>
#include <unordered_set>
#include <unordered_map>

namespace crystal {

// Crystal script opcode definitions
// AUTHORITATIVE SOURCE: pokecrystal/macros/scripts/events.asm
// Values are the actual ROM bytes - do NOT invent custom values
namespace CrystalOp {
    // Control flow (0x00-0x0D)
    inline constexpr uint8_t scall              = 0x00;  // dw pointer
    inline constexpr uint8_t farscall           = 0x01;  // dba pointer
    inline constexpr uint8_t memcall            = 0x02;  // dw pointer
    inline constexpr uint8_t sjump              = 0x03;  // dw pointer
    inline constexpr uint8_t farsjump           = 0x04;  // dba pointer
    inline constexpr uint8_t memjump            = 0x05;  // dw pointer
    inline constexpr uint8_t ifequal            = 0x06;  // db byte, dw pointer
    inline constexpr uint8_t ifnotequal         = 0x07;  // db byte, dw pointer
    inline constexpr uint8_t iffalse            = 0x08;  // dw pointer
    inline constexpr uint8_t iftrue             = 0x09;  // dw pointer
    inline constexpr uint8_t ifgreater          = 0x0A;  // db byte, dw pointer
    inline constexpr uint8_t ifless             = 0x0B;  // db byte, dw pointer
    inline constexpr uint8_t jumpstd            = 0x0C;  // dw std_id
    inline constexpr uint8_t callstd            = 0x0D;  // dw std_id
    
    // ASM/special (0x0E-0x10)
    inline constexpr uint8_t callasm            = 0x0E;  // dba asm
    inline constexpr uint8_t special            = 0x0F;  // dw special_id
    inline constexpr uint8_t memcallasm         = 0x10;  // dw asm
    
    // Map scene (0x11-0x14)
    inline constexpr uint8_t checkmapscene      = 0x11;  // map_id
    inline constexpr uint8_t setmapscene        = 0x12;  // map_id, db scene
    inline constexpr uint8_t checkscene         = 0x13;  // (no params)
    inline constexpr uint8_t setscene           = 0x14;  // db scene
    
    // Variables (0x15-0x1E)
    inline constexpr uint8_t setval             = 0x15;  // db value
    inline constexpr uint8_t addval             = 0x16;  // db value
    inline constexpr uint8_t random             = 0x17;  // db input
    inline constexpr uint8_t checkver           = 0x18;  // (no params)
    inline constexpr uint8_t readmem            = 0x19;  // dw address
    inline constexpr uint8_t writemem           = 0x1A;  // dw address
    inline constexpr uint8_t loadmem            = 0x1B;  // dw address, db value
    inline constexpr uint8_t readvar            = 0x1C;  // db var_id
    inline constexpr uint8_t writevar           = 0x1D;  // db var_id
    inline constexpr uint8_t loadvar            = 0x1E;  // db var_id, db value
    
    // Items (0x1F-0x27)
    inline constexpr uint8_t giveitem           = 0x1F;  // db item, db quantity
    inline constexpr uint8_t takeitem           = 0x20;  // db item, db quantity
    inline constexpr uint8_t checkitem          = 0x21;  // db item
    inline constexpr uint8_t givemoney          = 0x22;  // db account, bigdt money (3 bytes)
    inline constexpr uint8_t takemoney          = 0x23;  // db account, bigdt money
    inline constexpr uint8_t checkmoney         = 0x24;  // db account, bigdt money
    inline constexpr uint8_t givecoins          = 0x25;  // dw coins
    inline constexpr uint8_t takecoins          = 0x26;  // dw coins
    inline constexpr uint8_t checkcoins         = 0x27;  // dw coins
    
    // Phone (0x28-0x2A)
    inline constexpr uint8_t addcellnum         = 0x28;  // db person
    inline constexpr uint8_t delcellnum         = 0x29;  // db person
    inline constexpr uint8_t checkcellnum       = 0x2A;  // db person
    
    // Time/Pokemon (0x2B-0x2F)
    inline constexpr uint8_t checktime          = 0x2B;  // db time
    inline constexpr uint8_t checkpoke          = 0x2C;  // db pokemon
    inline constexpr uint8_t givepoke           = 0x2D;  // db pokemon, db level, db item, db trainer, [dw nick, dw ot]
    inline constexpr uint8_t giveegg            = 0x2E;  // db pokemon, db level
    inline constexpr uint8_t givepokemail       = 0x2F;  // dw pointer
    inline constexpr uint8_t checkpokemail      = 0x30;  // dw pointer
    
    // Events/Flags (0x31-0x36)
    inline constexpr uint8_t checkevent         = 0x31;  // dw event_flag
    inline constexpr uint8_t clearevent         = 0x32;  // dw event_flag
    inline constexpr uint8_t setevent           = 0x33;  // dw event_flag
    inline constexpr uint8_t checkflag          = 0x34;  // dw engine_flag
    inline constexpr uint8_t clearflag          = 0x35;  // dw engine_flag
    inline constexpr uint8_t setflag            = 0x36;  // dw engine_flag
    
    // Wild encounters (0x37-0x38)
    inline constexpr uint8_t wildon             = 0x37;  // (no params)
    inline constexpr uint8_t wildoff            = 0x38;  // (no params)
    
    // Map/warp (0x39-0x3C)
    inline constexpr uint8_t xycompare          = 0x39;  // dw pointer
    inline constexpr uint8_t warpmod            = 0x3A;  // db warp_id, map_id
    inline constexpr uint8_t blackoutmod        = 0x3B;  // map_id
    inline constexpr uint8_t warp               = 0x3C;  // map_id, db x, db y
    
    // String formatting (0x3D-0x44)
    inline constexpr uint8_t getmoney           = 0x3D;  // db strbuf, db account
    inline constexpr uint8_t getcoins           = 0x3E;  // db strbuf
    inline constexpr uint8_t getnum             = 0x3F;  // db strbuf
    inline constexpr uint8_t getmonname         = 0x40;  // db strbuf, db pokemon
    inline constexpr uint8_t getitemname        = 0x41;  // db strbuf, db item
    inline constexpr uint8_t getcurlandmarkname = 0x42;  // db strbuf
    inline constexpr uint8_t gettrainername     = 0x43;  // db strbuf, db trainer_group, db trainer_id
    inline constexpr uint8_t getstring          = 0x44;  // db strbuf, dw text_pointer
    
    // Items notify (0x45-0x46)
    inline constexpr uint8_t itemnotify         = 0x45;  // (no params)
    inline constexpr uint8_t pocketisfull       = 0x46;  // (no params)
    
    // Text (0x47-0x55)
    inline constexpr uint8_t opentext           = 0x47;  // (no params)
    inline constexpr uint8_t reanchormap        = 0x48;  // db dummy (optional)
    inline constexpr uint8_t closetext          = 0x49;  // (no params)
    inline constexpr uint8_t writeunusedbyte    = 0x4A;  // db byte
    inline constexpr uint8_t farwritetext       = 0x4B;  // dba pointer
    inline constexpr uint8_t writetext          = 0x4C;  // dw text_pointer
    inline constexpr uint8_t repeattext         = 0x4D;  // db byte, db byte
    inline constexpr uint8_t yesorno            = 0x4E;  // (no params)
    inline constexpr uint8_t loadmenu           = 0x4F;  // dw menu_header
    inline constexpr uint8_t closewindow        = 0x50;  // (no params)
    inline constexpr uint8_t jumptextfaceplayer = 0x51;  // dw text_pointer
    inline constexpr uint8_t farjumptext        = 0x52;  // dba pointer
    inline constexpr uint8_t jumptext           = 0x53;  // dw text_pointer
    inline constexpr uint8_t waitbutton         = 0x54;  // (no params)
    inline constexpr uint8_t promptbutton       = 0x55;  // (no params)
    
    // Pokemon display (0x56-0x5B)
    inline constexpr uint8_t pokepic            = 0x56;  // db pokemon
    inline constexpr uint8_t closepokepic       = 0x57;  // (no params)
    inline constexpr uint8_t _2dmenu            = 0x58;  // (no params)
    inline constexpr uint8_t verticalmenu       = 0x59;  // (no params)
    inline constexpr uint8_t loadpikachudata    = 0x5A;  // (no params)
    inline constexpr uint8_t randomwildmon      = 0x5B;  // (no params)
    
    // Battle setup (0x5C-0x67)
    inline constexpr uint8_t loadtemptrainer    = 0x5C;  // (no params)
    inline constexpr uint8_t loadwildmon        = 0x5D;  // db pokemon, db level
    inline constexpr uint8_t loadtrainer        = 0x5E;  // db trainer_group, db trainer_id
    inline constexpr uint8_t startbattle        = 0x5F;  // (no params)
    inline constexpr uint8_t reloadmapafterbattle = 0x60;  // (no params)
    inline constexpr uint8_t catchtutorial      = 0x61;  // db byte
    inline constexpr uint8_t trainertext        = 0x62;  // db text_id
    inline constexpr uint8_t trainerflagaction  = 0x63;  // db action
    inline constexpr uint8_t winlosstext        = 0x64;  // dw win_text, dw loss_text
    inline constexpr uint8_t scripttalkafter    = 0x65;  // (no params)
    inline constexpr uint8_t endifjustbattled   = 0x66;  // (no params)
    inline constexpr uint8_t checkjustbattled   = 0x67;  // (no params)
    
    // Movement (0x68-0x77)
    inline constexpr uint8_t setlasttalked      = 0x68;  // db object_id
    inline constexpr uint8_t applymovement      = 0x69;  // db object_id, dw data
    inline constexpr uint8_t applymovementlasttalked = 0x6A;  // dw data
    inline constexpr uint8_t faceplayer         = 0x6B;  // (no params)
    inline constexpr uint8_t faceobject         = 0x6C;  // db object1, db object2
    inline constexpr uint8_t variablesprite     = 0x6D;  // db slot, db sprite
    inline constexpr uint8_t disappear          = 0x6E;  // db object_id
    inline constexpr uint8_t appear             = 0x6F;  // db object_id
    inline constexpr uint8_t follow             = 0x70;  // db object2, db object1
    inline constexpr uint8_t stopfollow         = 0x71;  // (no params)
    inline constexpr uint8_t moveobject         = 0x72;  // db object_id, db x, db y
    inline constexpr uint8_t writeobjectxy      = 0x73;  // db object_id
    inline constexpr uint8_t loademote          = 0x74;  // db bubble
    inline constexpr uint8_t showemote          = 0x75;  // db bubble, db object_id, db time
    inline constexpr uint8_t turnobject         = 0x76;  // db object_id, db facing
    inline constexpr uint8_t follownotexact     = 0x77;  // db object2, db object1
    
    // Effects (0x78-0x7E)
    inline constexpr uint8_t earthquake         = 0x78;  // db param
    inline constexpr uint8_t changemapblocks    = 0x79;  // dba map_data_pointer
    inline constexpr uint8_t changeblock        = 0x7A;  // db x, db y, db block
    inline constexpr uint8_t reloadmap          = 0x7B;  // (no params)
    inline constexpr uint8_t refreshmap         = 0x7C;  // (no params)
    inline constexpr uint8_t writecmdqueue      = 0x7D;  // dw queue_pointer
    inline constexpr uint8_t delcmdqueue        = 0x7E;  // db byte
    
    // Audio (0x7F-0x88)
    inline constexpr uint8_t playmusic          = 0x7F;  // dw music
    inline constexpr uint8_t encountermusic     = 0x80;  // (no params)
    inline constexpr uint8_t musicfadeout       = 0x81;  // dw music, db fadetime
    inline constexpr uint8_t playmapmusic       = 0x82;  // (no params)
    inline constexpr uint8_t dontrestartmapmusic = 0x83;  // (no params)
    inline constexpr uint8_t cry                = 0x84;  // dw cry_id
    inline constexpr uint8_t playsound          = 0x85;  // dw sound
    inline constexpr uint8_t waitsfx            = 0x86;  // (no params)
    inline constexpr uint8_t warpsound          = 0x87;  // (no params)
    inline constexpr uint8_t specialsound       = 0x88;  // (no params)
    
    // Misc control (0x89-0x93)
    inline constexpr uint8_t autoinput          = 0x89;  // dba pointer
    inline constexpr uint8_t newloadmap         = 0x8A;  // db method
    inline constexpr uint8_t pause              = 0x8B;  // db length
    inline constexpr uint8_t deactivatefacing   = 0x8C;  // db time
    inline constexpr uint8_t sdefer             = 0x8D;  // dw pointer
    inline constexpr uint8_t warpcheck          = 0x8E;  // (no params)
    inline constexpr uint8_t stopandsjump       = 0x8F;  // dw pointer
    inline constexpr uint8_t endcallback        = 0x90;  // (no params)
    inline constexpr uint8_t end                = 0x91;  // (no params) - THE MAIN END COMMAND
    inline constexpr uint8_t reloadend          = 0x92;  // db method
    inline constexpr uint8_t endall             = 0x93;  // (no params)
    
    // Commerce (0x94-0x9D)
    inline constexpr uint8_t pokemart           = 0x94;  // db dialog_id, dw mart_id
    inline constexpr uint8_t elevator           = 0x95;  // dw floor_list
    inline constexpr uint8_t trade              = 0x96;  // db trade_id
    inline constexpr uint8_t askforphonenumber  = 0x97;  // db number
    inline constexpr uint8_t phonecall          = 0x98;  // dw caller_name
    inline constexpr uint8_t hangup             = 0x99;  // (no params)
    inline constexpr uint8_t describedecoration = 0x9A;  // db byte
    inline constexpr uint8_t fruittree          = 0x9B;  // db tree_id
    inline constexpr uint8_t specialphonecall   = 0x9C;  // dw call_id
    inline constexpr uint8_t checkphonecall     = 0x9D;  // (no params)
    
    // Verbose items (0x9E-0x9F)
    inline constexpr uint8_t verbosegiveitem    = 0x9E;  // db item, db quantity
    inline constexpr uint8_t verbosegiveitemvar = 0x9F;  // db item, db var
    
    // End game (0xA0-0xA9)
    inline constexpr uint8_t swarm              = 0xA0;  // db flag, map_id
    inline constexpr uint8_t halloffame         = 0xA1;  // (no params)
    inline constexpr uint8_t credits            = 0xA2;  // (no params)
    inline constexpr uint8_t warpfacing         = 0xA3;  // db facing, map_id, db x, db y
    inline constexpr uint8_t battletowertext    = 0xA4;  // db bttext_id
    inline constexpr uint8_t getlandmarkname    = 0xA5;  // db strbuf, db landmark_id
    inline constexpr uint8_t gettrainerclassname = 0xA6;  // db strbuf, db trainer_group
    inline constexpr uint8_t getname            = 0xA7;  // db strbuf, db type, db id
    inline constexpr uint8_t wait               = 0xA8;  // db duration
    inline constexpr uint8_t checksave          = 0xA9;  // (no params)
    
    inline constexpr uint8_t NUM_OPCODES        = 0xAA;
}

// Decoder state
struct DecoderContext {
    const RomData& rom;
    const SymbolMap& symbols;
    
    // Current position
    uint32_t pc = 0;
    
    // Current bank (for local pointers)
    uint8_t bank = 0;
    
    // Visited addresses (to detect loops/already-decoded)
    std::unordered_set<uint32_t> visited;
    
    // Pending addresses to decode (from jumps/calls)
    std::vector<uint32_t> pending;
};

// Decodes Crystal script bytecode
class ScriptDecoder {
public:
    ScriptDecoder(const RomData& rom, const SymbolMap& symbols);
    
    // Decode a single script starting at address
    ScriptIR decode_script(uint32_t address, const std::string& name = "");
    
    // Decode text from ROM address (legacy - returns flattened string)
    std::string decode_text(uint32_t address);
    
    // Decode text from ROM address (semantic - preserves LINE/CONT/PARA operations)
    TextSequence decode_text_sequence(uint32_t address);
    
    // Statistics
    struct Stats {
        size_t scripts_decoded = 0;
        size_t instructions_decoded = 0;
        size_t unknown_opcodes = 0;
        std::unordered_map<uint8_t, size_t> opcode_counts;
    };
    Stats get_stats() const { return stats_; }

private:
    const RomData& rom_;
    const SymbolMap& symbols_;
    Stats stats_;
    
    // Character map for text decoding (ROM byte -> UTF-8 string)
    std::unordered_map<uint8_t, std::string> charmap_;
    void init_charmap();
    
    // Decode helpers
    Instruction decode_instruction(DecoderContext& ctx);
    
    // Read helpers
    uint8_t read_byte(DecoderContext& ctx);
    uint16_t read_word(DecoderContext& ctx);
    uint32_t read_pointer(DecoderContext& ctx);  // 3-byte bank:addr pointer
    uint32_t read_local_pointer(DecoderContext& ctx);  // 2-byte local pointer (uses ctx.bank)
    
    // Map ID is 2 bytes: group (1) + map (1)
    uint16_t read_map_id(DecoderContext& ctx);
    
    // Resolve address to label
    LabelRef make_label_ref(uint32_t address);
    
    // All opcode handlers
    Operation decode_scall(DecoderContext& ctx);
    Operation decode_farscall(DecoderContext& ctx);
    Operation decode_sjump(DecoderContext& ctx);
    Operation decode_farsjump(DecoderContext& ctx);
    Operation decode_ifequal(DecoderContext& ctx);
    Operation decode_ifnotequal(DecoderContext& ctx);
    Operation decode_iffalse(DecoderContext& ctx);
    Operation decode_iftrue(DecoderContext& ctx);
    Operation decode_ifgreater(DecoderContext& ctx);
    Operation decode_ifless(DecoderContext& ctx);
    Operation decode_jumpstd(DecoderContext& ctx);
    Operation decode_callstd(DecoderContext& ctx);
    Operation decode_special(DecoderContext& ctx);
    Operation decode_setval(DecoderContext& ctx);
    Operation decode_addval(DecoderContext& ctx);
    Operation decode_checkevent(DecoderContext& ctx);
    Operation decode_clearevent(DecoderContext& ctx);
    Operation decode_setevent(DecoderContext& ctx);
    Operation decode_checkflag(DecoderContext& ctx);
    Operation decode_clearflag(DecoderContext& ctx);
    Operation decode_setflag(DecoderContext& ctx);
    Operation decode_giveitem(DecoderContext& ctx);
    Operation decode_takeitem(DecoderContext& ctx);
    Operation decode_checkitem(DecoderContext& ctx);
    Operation decode_givepoke(DecoderContext& ctx);
    Operation decode_opentext(DecoderContext& ctx);
    Operation decode_closetext(DecoderContext& ctx);
    Operation decode_writetext(DecoderContext& ctx);
    Operation decode_farwritetext(DecoderContext& ctx);
    Operation decode_jumptext(DecoderContext& ctx);
    Operation decode_farjumptext(DecoderContext& ctx);
    Operation decode_jumptextfaceplayer(DecoderContext& ctx);
    Operation decode_waitbutton(DecoderContext& ctx);
    Operation decode_yesorno(DecoderContext& ctx);
    Operation decode_faceplayer(DecoderContext& ctx);
    Operation decode_applymovement(DecoderContext& ctx);
    Operation decode_applymovementlasttalked(DecoderContext& ctx);
    Operation decode_turnobject(DecoderContext& ctx);
    Operation decode_appear(DecoderContext& ctx);
    Operation decode_disappear(DecoderContext& ctx);
    Operation decode_warp(DecoderContext& ctx);
    Operation decode_warpfacing(DecoderContext& ctx);
    Operation decode_playmusic(DecoderContext& ctx);
    Operation decode_playsound(DecoderContext& ctx);
    Operation decode_cry(DecoderContext& ctx);
    Operation decode_waitsfx(DecoderContext& ctx);
    Operation decode_pause(DecoderContext& ctx);
    Operation decode_wait(DecoderContext& ctx);
    Operation decode_end(DecoderContext& ctx);
    Operation decode_endcallback(DecoderContext& ctx);
    
    // Movement data decoder
    std::vector<uint8_t> decode_movement_data(uint32_t address);
    
public:
    // Parse raw movement bytes into semantic commands (public for testing)
    std::vector<MovementCommand> parse_movement_commands(const std::vector<uint8_t>& raw);
};

} // namespace crystal
