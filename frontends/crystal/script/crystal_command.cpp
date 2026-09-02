// crystal/script/crystal_command.cpp
// CrystalCommand implementation including round-trip encoding validation
// NOTE: Uses multiple encode_xxx functions to avoid MSVC nesting limit

#include "crystal/script/crystal_command.hpp"
#include "crystal/script/decoder.hpp"  // For CrystalOp constants
#include <vector>

namespace crystal {

// =============================================================================
// CrystalCommand + CrystalScriptIR — out-of-line special members
//
// Declaring these non-inline in the header and defining them here means
// CrystalCommandData (171-alt) destructor/move/copy machinery is only
// generated in this TU (which already pays the cost via std::visit in
// is_terminator/is_branch/is_call). Every other TU that includes
// crystal_command.hpp no longer instantiates the variant machinery.
// =============================================================================

CrystalCommand::CrystalCommand()                                          = default;
CrystalCommand::~CrystalCommand()                                         = default;
CrystalCommand::CrystalCommand(const CrystalCommand&)                     = default;
CrystalCommand::CrystalCommand(CrystalCommand&&) noexcept                 = default;
CrystalCommand& CrystalCommand::operator=(const CrystalCommand&)          = default;
CrystalCommand& CrystalCommand::operator=(CrystalCommand&&) noexcept      = default;

CrystalScriptIR::CrystalScriptIR()                                        = default;
CrystalScriptIR::~CrystalScriptIR()                                       = default;
CrystalScriptIR::CrystalScriptIR(const CrystalScriptIR&)                  = default;
CrystalScriptIR::CrystalScriptIR(CrystalScriptIR&&) noexcept              = default;
CrystalScriptIR& CrystalScriptIR::operator=(const CrystalScriptIR&)       = default;
CrystalScriptIR& CrystalScriptIR::operator=(CrystalScriptIR&&) noexcept   = default;

// Explicit instantiation moved to file scope — see bottom of file.
bool CrystalCommand::is_terminator() const {
    return std::visit([](const auto& cmd) -> bool {
        using T = std::decay_t<decltype(cmd)>;
        if constexpr (std::is_same_v<T, Cmd_End>) return true;
        if constexpr (std::is_same_v<T, Cmd_Endall>) return true;
        if constexpr (std::is_same_v<T, Cmd_Endcallback>) return true;
        if constexpr (std::is_same_v<T, Cmd_Sjump>) return true;
        if constexpr (std::is_same_v<T, Cmd_Farsjump>) return true;
        if constexpr (std::is_same_v<T, Cmd_Memjump>) return true;
        if constexpr (std::is_same_v<T, Cmd_Jumpstd>) return true;
        if constexpr (std::is_same_v<T, Cmd_Jumptext>) return true;
        if constexpr (std::is_same_v<T, Cmd_Farjumptext>) return true;
        if constexpr (std::is_same_v<T, Cmd_Jumptextfaceplayer>) return true;
        if constexpr (std::is_same_v<T, Cmd_Stopandsjump>) return true;
        if constexpr (std::is_same_v<T, Cmd_Reloadend>) return true;
        // Commands that use jp ScriptJump (tail-transfer to another script):
        // Reference: pokecrystal/engine/overworld/scripting.asm
        if constexpr (std::is_same_v<T, Cmd_Fruittree>) return true;         // 0x9b - jp ScriptJump to FruitTreeScript
        if constexpr (std::is_same_v<T, Cmd_Describedecoration>) return true; // 0x9a - jp ScriptJump after DescribeDecoration
        if constexpr (std::is_same_v<T, Cmd_Scripttalkafter>) return true;    // 0x65 - jp ScriptJump to wScriptAfterPointer
        // Unknown opcodes terminate to prevent infinite loops
        if constexpr (std::is_same_v<T, Cmd_Unknown>) return true;
        return false;
    }, data);
}

bool CrystalCommand::is_branch() const {
    return std::visit([](const auto& cmd) -> bool {
        using T = std::decay_t<decltype(cmd)>;
        if constexpr (std::is_same_v<T, Cmd_Ifequal>) return true;
        if constexpr (std::is_same_v<T, Cmd_Ifnotequal>) return true;
        if constexpr (std::is_same_v<T, Cmd_Iffalse>) return true;
        if constexpr (std::is_same_v<T, Cmd_Iftrue>) return true;
        if constexpr (std::is_same_v<T, Cmd_Ifgreater>) return true;
        if constexpr (std::is_same_v<T, Cmd_Ifless>) return true;
        return false;
    }, data);
}

bool CrystalCommand::is_call() const {
    return std::visit([](const auto& cmd) -> bool {
        using T = std::decay_t<decltype(cmd)>;
        if constexpr (std::is_same_v<T, Cmd_Scall>) return true;
        if constexpr (std::is_same_v<T, Cmd_Farscall>) return true;
        if constexpr (std::is_same_v<T, Cmd_Memcall>) return true;
        if constexpr (std::is_same_v<T, Cmd_Callstd>) return true;
        if constexpr (std::is_same_v<T, Cmd_Callasm>) return true;
        if constexpr (std::is_same_v<T, Cmd_Memcallasm>) return true;
        return false;
    }, data);
}

size_t CrystalScriptIR::unknown_count() const {
    size_t count = 0;
    for (const auto& cmd : commands) {
        if (std::holds_alternative<Cmd_Unknown>(cmd.data)) {
            count++;
        }
    }
    return count;
}

// Helper to push bytes
static void push_byte(std::vector<uint8_t>& out, uint8_t b) {
    out.push_back(b);
}

static void push_word(std::vector<uint8_t>& out, uint16_t w) {
    out.push_back(static_cast<uint8_t>(w & 0xFF));
    out.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
}

static void push_map_id(std::vector<uint8_t>& out, const CrystalMapId& m) {
    out.push_back(m.group);
    out.push_back(m.map);
}

// Split encoding into groups to avoid MSVC nesting limit
// Group 1: Control flow (0x00-0x0D)
static void encode_control_flow(std::vector<uint8_t>& out, const CrystalCommandData& data) {
    if (auto* c = std::get_if<Cmd_Scall>(&data)) {
        push_byte(out, CrystalOp::scall); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Farscall>(&data)) {
        push_byte(out, CrystalOp::farscall); push_byte(out, c->bank); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Memcall>(&data)) {
        push_byte(out, CrystalOp::memcall); push_word(out, c->ram_address);
    } else if (auto* c = std::get_if<Cmd_Sjump>(&data)) {
        push_byte(out, CrystalOp::sjump); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Farsjump>(&data)) {
        push_byte(out, CrystalOp::farsjump); push_byte(out, c->bank); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Memjump>(&data)) {
        push_byte(out, CrystalOp::memjump); push_word(out, c->ram_address);
    } else if (auto* c = std::get_if<Cmd_Ifequal>(&data)) {
        push_byte(out, CrystalOp::ifequal); push_byte(out, c->value); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Ifnotequal>(&data)) {
        push_byte(out, CrystalOp::ifnotequal); push_byte(out, c->value); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Iffalse>(&data)) {
        push_byte(out, CrystalOp::iffalse); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Iftrue>(&data)) {
        push_byte(out, CrystalOp::iftrue); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Ifgreater>(&data)) {
        push_byte(out, CrystalOp::ifgreater); push_byte(out, c->value); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Ifless>(&data)) {
        push_byte(out, CrystalOp::ifless); push_byte(out, c->value); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Jumpstd>(&data)) {
        push_byte(out, CrystalOp::jumpstd); push_word(out, c->std_id);
    } else if (auto* c = std::get_if<Cmd_Callstd>(&data)) {
        push_byte(out, CrystalOp::callstd); push_word(out, c->std_id);
    }
}

// Group 2: ASM/Special + Map scene + Variables (0x0E-0x1E)
static void encode_asm_scene_vars(std::vector<uint8_t>& out, const CrystalCommandData& data) {
    if (auto* c = std::get_if<Cmd_Callasm>(&data)) {
        push_byte(out, CrystalOp::callasm); push_byte(out, c->bank); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Special>(&data)) {
        push_byte(out, CrystalOp::special); push_word(out, c->special_id);
    } else if (auto* c = std::get_if<Cmd_Memcallasm>(&data)) {
        push_byte(out, CrystalOp::memcallasm); push_word(out, c->ram_address);
    } else if (auto* c = std::get_if<Cmd_Checkmapscene>(&data)) {
        push_byte(out, CrystalOp::checkmapscene); push_map_id(out, c->map);
    } else if (auto* c = std::get_if<Cmd_Setmapscene>(&data)) {
        push_byte(out, CrystalOp::setmapscene); push_map_id(out, c->map); push_byte(out, c->scene);
    } else if (std::get_if<Cmd_Checkscene>(&data)) {
        push_byte(out, CrystalOp::checkscene);
    } else if (auto* c = std::get_if<Cmd_Setscene>(&data)) {
        push_byte(out, CrystalOp::setscene); push_byte(out, c->scene);
    } else if (auto* c = std::get_if<Cmd_Setval>(&data)) {
        push_byte(out, CrystalOp::setval); push_byte(out, c->value);
    } else if (auto* c = std::get_if<Cmd_Addval>(&data)) {
        push_byte(out, CrystalOp::addval); push_byte(out, c->value);
    } else if (auto* c = std::get_if<Cmd_Random>(&data)) {
        push_byte(out, CrystalOp::random); push_byte(out, c->range);
    } else if (std::get_if<Cmd_Checkver>(&data)) {
        push_byte(out, CrystalOp::checkver);
    } else if (auto* c = std::get_if<Cmd_Readmem>(&data)) {
        push_byte(out, CrystalOp::readmem); push_word(out, c->ram_address);
    } else if (auto* c = std::get_if<Cmd_Writemem>(&data)) {
        push_byte(out, CrystalOp::writemem); push_word(out, c->ram_address);
    } else if (auto* c = std::get_if<Cmd_Loadmem>(&data)) {
        push_byte(out, CrystalOp::loadmem); push_word(out, c->ram_address); push_byte(out, c->value);
    } else if (auto* c = std::get_if<Cmd_Readvar>(&data)) {
        push_byte(out, CrystalOp::readvar); push_byte(out, c->var_id);
    } else if (auto* c = std::get_if<Cmd_Writevar>(&data)) {
        push_byte(out, CrystalOp::writevar); push_byte(out, c->var_id);
    } else if (auto* c = std::get_if<Cmd_Loadvar>(&data)) {
        push_byte(out, CrystalOp::loadvar); push_byte(out, c->var_id); push_byte(out, c->value);
    }
}

// Group 3: Items + Phone + Time/Pokemon (0x1F-0x30)
static void encode_items_phone_pokemon(std::vector<uint8_t>& out, const CrystalCommandData& data) {
    if (auto* c = std::get_if<Cmd_Giveitem>(&data)) {
        push_byte(out, CrystalOp::giveitem); push_byte(out, c->item); push_byte(out, c->quantity);
    } else if (auto* c = std::get_if<Cmd_Takeitem>(&data)) {
        push_byte(out, CrystalOp::takeitem); push_byte(out, c->item); push_byte(out, c->quantity);
    } else if (auto* c = std::get_if<Cmd_Checkitem>(&data)) {
        push_byte(out, CrystalOp::checkitem); push_byte(out, c->item);
    } else if (auto* c = std::get_if<Cmd_Givemoney>(&data)) {
        push_byte(out, CrystalOp::givemoney); push_byte(out, c->account);
        push_byte(out, c->money_byte1); push_byte(out, c->money_byte2); push_byte(out, c->money_byte3);
    } else if (auto* c = std::get_if<Cmd_Takemoney>(&data)) {
        push_byte(out, CrystalOp::takemoney); push_byte(out, c->account);
        push_byte(out, c->money_byte1); push_byte(out, c->money_byte2); push_byte(out, c->money_byte3);
    } else if (auto* c = std::get_if<Cmd_Checkmoney>(&data)) {
        push_byte(out, CrystalOp::checkmoney); push_byte(out, c->account);
        push_byte(out, c->money_byte1); push_byte(out, c->money_byte2); push_byte(out, c->money_byte3);
    } else if (auto* c = std::get_if<Cmd_Givecoins>(&data)) {
        push_byte(out, CrystalOp::givecoins); push_word(out, c->coins);
    } else if (auto* c = std::get_if<Cmd_Takecoins>(&data)) {
        push_byte(out, CrystalOp::takecoins); push_word(out, c->coins);
    } else if (auto* c = std::get_if<Cmd_Checkcoins>(&data)) {
        push_byte(out, CrystalOp::checkcoins); push_word(out, c->coins);
    } else if (auto* c = std::get_if<Cmd_Addcellnum>(&data)) {
        push_byte(out, CrystalOp::addcellnum); push_byte(out, c->person);
    } else if (auto* c = std::get_if<Cmd_Delcellnum>(&data)) {
        push_byte(out, CrystalOp::delcellnum); push_byte(out, c->person);
    } else if (auto* c = std::get_if<Cmd_Checkcellnum>(&data)) {
        push_byte(out, CrystalOp::checkcellnum); push_byte(out, c->person);
    } else if (auto* c = std::get_if<Cmd_Checktime>(&data)) {
        push_byte(out, CrystalOp::checktime); push_byte(out, c->time);
    } else if (auto* c = std::get_if<Cmd_Checkpoke>(&data)) {
        push_byte(out, CrystalOp::checkpoke); push_byte(out, c->pokemon);
    } else if (auto* c = std::get_if<Cmd_Givepoke>(&data)) {
        push_byte(out, CrystalOp::givepoke); push_byte(out, c->pokemon);
        push_byte(out, c->level); push_byte(out, c->item); push_byte(out, c->trainer);
        if (c->has_extra_data) { push_word(out, c->nickname_ptr); push_word(out, c->ot_name_ptr); }
    } else if (auto* c = std::get_if<Cmd_Giveegg>(&data)) {
        push_byte(out, CrystalOp::giveegg); push_byte(out, c->pokemon); push_byte(out, c->level);
    } else if (auto* c = std::get_if<Cmd_Givepokemail>(&data)) {
        push_byte(out, CrystalOp::givepokemail); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Checkpokemail>(&data)) {
        push_byte(out, CrystalOp::checkpokemail); push_word(out, c->pointer);
    }
}

// Group 4: Events/Flags + Wild + Map/Warp (0x31-0x3C)
static void encode_events_flags_warp(std::vector<uint8_t>& out, const CrystalCommandData& data) {
    if (auto* c = std::get_if<Cmd_Checkevent>(&data)) {
        push_byte(out, CrystalOp::checkevent); push_word(out, c->event_flag);
    } else if (auto* c = std::get_if<Cmd_Clearevent>(&data)) {
        push_byte(out, CrystalOp::clearevent); push_word(out, c->event_flag);
    } else if (auto* c = std::get_if<Cmd_Setevent>(&data)) {
        push_byte(out, CrystalOp::setevent); push_word(out, c->event_flag);
    } else if (auto* c = std::get_if<Cmd_Checkflag>(&data)) {
        push_byte(out, CrystalOp::checkflag); push_word(out, c->engine_flag);
    } else if (auto* c = std::get_if<Cmd_Clearflag>(&data)) {
        push_byte(out, CrystalOp::clearflag); push_word(out, c->engine_flag);
    } else if (auto* c = std::get_if<Cmd_Setflag>(&data)) {
        push_byte(out, CrystalOp::setflag); push_word(out, c->engine_flag);
    } else if (std::get_if<Cmd_Wildon>(&data)) {
        push_byte(out, CrystalOp::wildon);
    } else if (std::get_if<Cmd_Wildoff>(&data)) {
        push_byte(out, CrystalOp::wildoff);
    } else if (auto* c = std::get_if<Cmd_Xycompare>(&data)) {
        push_byte(out, CrystalOp::xycompare); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Warpmod>(&data)) {
        push_byte(out, CrystalOp::warpmod); push_byte(out, c->warp_id); push_map_id(out, c->map);
    } else if (auto* c = std::get_if<Cmd_Blackoutmod>(&data)) {
        push_byte(out, CrystalOp::blackoutmod); push_map_id(out, c->map);
    } else if (auto* c = std::get_if<Cmd_Warp>(&data)) {
        push_byte(out, CrystalOp::warp); push_map_id(out, c->map); push_byte(out, c->x); push_byte(out, c->y);
    }
}

// Group 5: String formatting + Item notify (0x3D-0x46)
// AUTHORITATIVE SOURCE: pokecrystal/macros/scripts/events.asm
// ROM byte order differs from macro argument order - data operands come FIRST, strbuf LAST
static void encode_string_itemnotify(std::vector<uint8_t>& out, const CrystalCommandData& data) {
    if (auto* c = std::get_if<Cmd_Getmoney>(&data)) {
        // ROM: db account, db strbuf
        push_byte(out, CrystalOp::getmoney); push_byte(out, c->account); push_byte(out, c->strbuf);
    } else if (auto* c = std::get_if<Cmd_Getcoins>(&data)) {
        push_byte(out, CrystalOp::getcoins); push_byte(out, c->strbuf);
    } else if (auto* c = std::get_if<Cmd_Getnum>(&data)) {
        push_byte(out, CrystalOp::getnum); push_byte(out, c->strbuf);
    } else if (auto* c = std::get_if<Cmd_Getmonname>(&data)) {
        // ROM: db pokemon, db strbuf
        push_byte(out, CrystalOp::getmonname); push_byte(out, c->pokemon); push_byte(out, c->strbuf);
    } else if (auto* c = std::get_if<Cmd_Getitemname>(&data)) {
        // ROM: db item, db strbuf
        push_byte(out, CrystalOp::getitemname); push_byte(out, c->item); push_byte(out, c->strbuf);
    } else if (auto* c = std::get_if<Cmd_Getcurlandmarkname>(&data)) {
        push_byte(out, CrystalOp::getcurlandmarkname); push_byte(out, c->strbuf);
    } else if (auto* c = std::get_if<Cmd_Gettrainername>(&data)) {
        // ROM: db trainer_group, db trainer_id, db strbuf
        push_byte(out, CrystalOp::gettrainername);
        push_byte(out, c->trainer_group); push_byte(out, c->trainer_id); push_byte(out, c->strbuf);
    } else if (auto* c = std::get_if<Cmd_Getstring>(&data)) {
        // ROM: dw text_pointer, db strbuf
        push_byte(out, CrystalOp::getstring); push_word(out, c->text_pointer); push_byte(out, c->strbuf);
    } else if (std::get_if<Cmd_Itemnotify>(&data)) {
        push_byte(out, CrystalOp::itemnotify);
    } else if (std::get_if<Cmd_Pocketisfull>(&data)) {
        push_byte(out, CrystalOp::pocketisfull);
    }
}

// Group 6: Text commands (0x47-0x55)
static void encode_text(std::vector<uint8_t>& out, const CrystalCommandData& data) {
    if (std::get_if<Cmd_Opentext>(&data)) {
        push_byte(out, CrystalOp::opentext);
    } else if (auto* c = std::get_if<Cmd_Reanchormap>(&data)) {
        push_byte(out, CrystalOp::reanchormap); push_byte(out, c->dummy);
    } else if (std::get_if<Cmd_Closetext>(&data)) {
        push_byte(out, CrystalOp::closetext);
    } else if (auto* c = std::get_if<Cmd_Writeunusedbyte>(&data)) {
        push_byte(out, CrystalOp::writeunusedbyte); push_byte(out, c->byte);
    } else if (auto* c = std::get_if<Cmd_Farwritetext>(&data)) {
        push_byte(out, CrystalOp::farwritetext); push_byte(out, c->bank); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Writetext>(&data)) {
        push_byte(out, CrystalOp::writetext); push_word(out, c->text_pointer);
    } else if (auto* c = std::get_if<Cmd_Repeattext>(&data)) {
        push_byte(out, CrystalOp::repeattext); push_byte(out, c->byte1); push_byte(out, c->byte2);
    } else if (std::get_if<Cmd_Yesorno>(&data)) {
        push_byte(out, CrystalOp::yesorno);
    } else if (auto* c = std::get_if<Cmd_Loadmenu>(&data)) {
        push_byte(out, CrystalOp::loadmenu); push_word(out, c->menu_header);
    } else if (std::get_if<Cmd_Closewindow>(&data)) {
        push_byte(out, CrystalOp::closewindow);
    } else if (auto* c = std::get_if<Cmd_Jumptextfaceplayer>(&data)) {
        push_byte(out, CrystalOp::jumptextfaceplayer); push_word(out, c->text_pointer);
    } else if (auto* c = std::get_if<Cmd_Farjumptext>(&data)) {
        push_byte(out, CrystalOp::farjumptext); push_byte(out, c->bank); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Jumptext>(&data)) {
        push_byte(out, CrystalOp::jumptext); push_word(out, c->text_pointer);
    } else if (std::get_if<Cmd_Waitbutton>(&data)) {
        push_byte(out, CrystalOp::waitbutton);
    } else if (std::get_if<Cmd_Promptbutton>(&data)) {
        push_byte(out, CrystalOp::promptbutton);
    }
}

// Group 7: Pokemon display + Battle setup (0x56-0x67)
static void encode_display_battle(std::vector<uint8_t>& out, const CrystalCommandData& data) {
    if (auto* c = std::get_if<Cmd_Pokepic>(&data)) {
        push_byte(out, CrystalOp::pokepic); push_byte(out, c->pokemon);
    } else if (std::get_if<Cmd_Closepokepic>(&data)) {
        push_byte(out, CrystalOp::closepokepic);
    } else if (std::get_if<Cmd_2dmenu>(&data)) {
        push_byte(out, CrystalOp::_2dmenu);
    } else if (std::get_if<Cmd_Verticalmenu>(&data)) {
        push_byte(out, CrystalOp::verticalmenu);
    } else if (std::get_if<Cmd_Loadpikachudata>(&data)) {
        push_byte(out, CrystalOp::loadpikachudata);
    } else if (std::get_if<Cmd_Randomwildmon>(&data)) {
        push_byte(out, CrystalOp::randomwildmon);
    } else if (std::get_if<Cmd_Loadtemptrainer>(&data)) {
        push_byte(out, CrystalOp::loadtemptrainer);
    } else if (auto* c = std::get_if<Cmd_Loadwildmon>(&data)) {
        push_byte(out, CrystalOp::loadwildmon); push_byte(out, c->pokemon); push_byte(out, c->level);
    } else if (auto* c = std::get_if<Cmd_Loadtrainer>(&data)) {
        push_byte(out, CrystalOp::loadtrainer); push_byte(out, c->trainer_group); push_byte(out, c->trainer_id);
    } else if (std::get_if<Cmd_Startbattle>(&data)) {
        push_byte(out, CrystalOp::startbattle);
    } else if (std::get_if<Cmd_Reloadmapafterbattle>(&data)) {
        push_byte(out, CrystalOp::reloadmapafterbattle);
    } else if (auto* c = std::get_if<Cmd_Catchtutorial>(&data)) {
        push_byte(out, CrystalOp::catchtutorial); push_byte(out, c->byte);
    } else if (auto* c = std::get_if<Cmd_Trainertext>(&data)) {
        push_byte(out, CrystalOp::trainertext); push_byte(out, c->text_id);
    } else if (auto* c = std::get_if<Cmd_Trainerflagaction>(&data)) {
        push_byte(out, CrystalOp::trainerflagaction); push_byte(out, c->action);
    } else if (auto* c = std::get_if<Cmd_Winlosstext>(&data)) {
        push_byte(out, CrystalOp::winlosstext); push_word(out, c->win_text); push_word(out, c->loss_text);
    } else if (std::get_if<Cmd_Scripttalkafter>(&data)) {
        push_byte(out, CrystalOp::scripttalkafter);
    } else if (std::get_if<Cmd_Endifjustbattled>(&data)) {
        push_byte(out, CrystalOp::endifjustbattled);
    } else if (std::get_if<Cmd_Checkjustbattled>(&data)) {
        push_byte(out, CrystalOp::checkjustbattled);
    }
}

// Group 8: Movement (0x68-0x77)
static void encode_movement(std::vector<uint8_t>& out, const CrystalCommandData& data) {
    if (auto* c = std::get_if<Cmd_Setlasttalked>(&data)) {
        push_byte(out, CrystalOp::setlasttalked); push_byte(out, c->object_id);
    } else if (auto* c = std::get_if<Cmd_Applymovement>(&data)) {
        push_byte(out, CrystalOp::applymovement); push_byte(out, c->object_id); push_word(out, c->movement_pointer);
    } else if (auto* c = std::get_if<Cmd_Applymovementlasttalked>(&data)) {
        push_byte(out, CrystalOp::applymovementlasttalked); push_word(out, c->movement_pointer);
    } else if (std::get_if<Cmd_Faceplayer>(&data)) {
        push_byte(out, CrystalOp::faceplayer);
    } else if (auto* c = std::get_if<Cmd_Faceobject>(&data)) {
        push_byte(out, CrystalOp::faceobject); push_byte(out, c->object1); push_byte(out, c->object2);
    } else if (auto* c = std::get_if<Cmd_Variablesprite>(&data)) {
        push_byte(out, CrystalOp::variablesprite); push_byte(out, c->slot); push_byte(out, c->sprite);
    } else if (auto* c = std::get_if<Cmd_Disappear>(&data)) {
        push_byte(out, CrystalOp::disappear); push_byte(out, c->object_id);
    } else if (auto* c = std::get_if<Cmd_Appear>(&data)) {
        push_byte(out, CrystalOp::appear); push_byte(out, c->object_id);
    } else if (auto* c = std::get_if<Cmd_Follow>(&data)) {
        push_byte(out, CrystalOp::follow); push_byte(out, c->object2); push_byte(out, c->object1);
    } else if (std::get_if<Cmd_Stopfollow>(&data)) {
        push_byte(out, CrystalOp::stopfollow);
    } else if (auto* c = std::get_if<Cmd_Moveobject>(&data)) {
        push_byte(out, CrystalOp::moveobject); push_byte(out, c->object_id);
        push_byte(out, c->x); push_byte(out, c->y);
    } else if (auto* c = std::get_if<Cmd_Writeobjectxy>(&data)) {
        push_byte(out, CrystalOp::writeobjectxy); push_byte(out, c->object_id);
    } else if (auto* c = std::get_if<Cmd_Loademote>(&data)) {
        push_byte(out, CrystalOp::loademote); push_byte(out, c->bubble);
    } else if (auto* c = std::get_if<Cmd_Showemote>(&data)) {
        push_byte(out, CrystalOp::showemote); push_byte(out, c->bubble);
        push_byte(out, c->object_id); push_byte(out, c->time);
    } else if (auto* c = std::get_if<Cmd_Turnobject>(&data)) {
        push_byte(out, CrystalOp::turnobject); push_byte(out, c->object_id); push_byte(out, c->facing);
    } else if (auto* c = std::get_if<Cmd_Follownotexact>(&data)) {
        push_byte(out, CrystalOp::follownotexact); push_byte(out, c->object2); push_byte(out, c->object1);
    }
}

// Group 9: Effects + Audio (0x78-0x88)
static void encode_effects_audio(std::vector<uint8_t>& out, const CrystalCommandData& data) {
    if (auto* c = std::get_if<Cmd_Earthquake>(&data)) {
        push_byte(out, CrystalOp::earthquake); push_byte(out, c->param);
    } else if (auto* c = std::get_if<Cmd_Changemapblocks>(&data)) {
        push_byte(out, CrystalOp::changemapblocks); push_byte(out, c->bank); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Changeblock>(&data)) {
        push_byte(out, CrystalOp::changeblock); push_byte(out, c->x); push_byte(out, c->y); push_byte(out, c->block);
    } else if (std::get_if<Cmd_Reloadmap>(&data)) {
        push_byte(out, CrystalOp::reloadmap);
    } else if (std::get_if<Cmd_Refreshmap>(&data)) {
        push_byte(out, CrystalOp::refreshmap);
    } else if (auto* c = std::get_if<Cmd_Writecmdqueue>(&data)) {
        push_byte(out, CrystalOp::writecmdqueue); push_word(out, c->queue_pointer);
    } else if (auto* c = std::get_if<Cmd_Delcmdqueue>(&data)) {
        push_byte(out, CrystalOp::delcmdqueue); push_byte(out, c->byte);
    } else if (auto* c = std::get_if<Cmd_Playmusic>(&data)) {
        push_byte(out, CrystalOp::playmusic); push_word(out, c->music);
    } else if (std::get_if<Cmd_Encountermusic>(&data)) {
        push_byte(out, CrystalOp::encountermusic);
    } else if (auto* c = std::get_if<Cmd_Musicfadeout>(&data)) {
        push_byte(out, CrystalOp::musicfadeout); push_word(out, c->music); push_byte(out, c->fadetime);
    } else if (std::get_if<Cmd_Playmapmusic>(&data)) {
        push_byte(out, CrystalOp::playmapmusic);
    } else if (std::get_if<Cmd_Dontrestartmapmusic>(&data)) {
        push_byte(out, CrystalOp::dontrestartmapmusic);
    } else if (auto* c = std::get_if<Cmd_Cry>(&data)) {
        push_byte(out, CrystalOp::cry); push_word(out, c->cry_id);
    } else if (auto* c = std::get_if<Cmd_Playsound>(&data)) {
        push_byte(out, CrystalOp::playsound); push_word(out, c->sound);
    } else if (std::get_if<Cmd_Waitsfx>(&data)) {
        push_byte(out, CrystalOp::waitsfx);
    } else if (std::get_if<Cmd_Warpsound>(&data)) {
        push_byte(out, CrystalOp::warpsound);
    } else if (std::get_if<Cmd_Specialsound>(&data)) {
        push_byte(out, CrystalOp::specialsound);
    }
}

// Group 10: Misc control (0x89-0x93)
static void encode_misc_control(std::vector<uint8_t>& out, const CrystalCommandData& data) {
    if (auto* c = std::get_if<Cmd_Autoinput>(&data)) {
        push_byte(out, CrystalOp::autoinput); push_byte(out, c->bank); push_word(out, c->pointer);
    } else if (auto* c = std::get_if<Cmd_Newloadmap>(&data)) {
        push_byte(out, CrystalOp::newloadmap); push_byte(out, c->method);
    } else if (auto* c = std::get_if<Cmd_Pause>(&data)) {
        push_byte(out, CrystalOp::pause); push_byte(out, c->length);
    } else if (auto* c = std::get_if<Cmd_Deactivatefacing>(&data)) {
        push_byte(out, CrystalOp::deactivatefacing); push_byte(out, c->time);
    } else if (auto* c = std::get_if<Cmd_Sdefer>(&data)) {
        push_byte(out, CrystalOp::sdefer); push_word(out, c->pointer);
    } else if (std::get_if<Cmd_Warpcheck>(&data)) {
        push_byte(out, CrystalOp::warpcheck);
    } else if (auto* c = std::get_if<Cmd_Stopandsjump>(&data)) {
        push_byte(out, CrystalOp::stopandsjump); push_word(out, c->pointer);
    } else if (std::get_if<Cmd_Endcallback>(&data)) {
        push_byte(out, CrystalOp::endcallback);
    } else if (std::get_if<Cmd_End>(&data)) {
        push_byte(out, CrystalOp::end);
    } else if (auto* c = std::get_if<Cmd_Reloadend>(&data)) {
        push_byte(out, CrystalOp::reloadend); push_byte(out, c->method);
    } else if (std::get_if<Cmd_Endall>(&data)) {
        push_byte(out, CrystalOp::endall);
    }
}

// Group 11: Commerce + Verbose items (0x94-0x9F)
static void encode_commerce(std::vector<uint8_t>& out, const CrystalCommandData& data) {
    if (auto* c = std::get_if<Cmd_Pokemart>(&data)) {
        push_byte(out, CrystalOp::pokemart); push_byte(out, c->dialog_id); push_word(out, c->mart_id);
    } else if (auto* c = std::get_if<Cmd_Elevator>(&data)) {
        push_byte(out, CrystalOp::elevator); push_word(out, c->floor_list);
    } else if (auto* c = std::get_if<Cmd_Trade>(&data)) {
        push_byte(out, CrystalOp::trade); push_byte(out, c->trade_id);
    } else if (auto* c = std::get_if<Cmd_Askforphonenumber>(&data)) {
        push_byte(out, CrystalOp::askforphonenumber); push_byte(out, c->number);
    } else if (auto* c = std::get_if<Cmd_Phonecall>(&data)) {
        push_byte(out, CrystalOp::phonecall); push_word(out, c->caller_name);
    } else if (std::get_if<Cmd_Hangup>(&data)) {
        push_byte(out, CrystalOp::hangup);
    } else if (auto* c = std::get_if<Cmd_Describedecoration>(&data)) {
        push_byte(out, CrystalOp::describedecoration); push_byte(out, c->byte);
    } else if (auto* c = std::get_if<Cmd_Fruittree>(&data)) {
        push_byte(out, CrystalOp::fruittree); push_byte(out, c->tree_id);
    } else if (auto* c = std::get_if<Cmd_Specialphonecall>(&data)) {
        push_byte(out, CrystalOp::specialphonecall); push_word(out, c->call_id);
    } else if (std::get_if<Cmd_Checkphonecall>(&data)) {
        push_byte(out, CrystalOp::checkphonecall);
    } else if (auto* c = std::get_if<Cmd_Verbosegiveitem>(&data)) {
        push_byte(out, CrystalOp::verbosegiveitem); push_byte(out, c->item); push_byte(out, c->quantity);
    } else if (auto* c = std::get_if<Cmd_Verbosegiveitemvar>(&data)) {
        push_byte(out, CrystalOp::verbosegiveitemvar); push_byte(out, c->item); push_byte(out, c->var);
    }
}

// Group 12: End game + Unknown (0xA0-0xA9+)
// AUTHORITATIVE SOURCE: pokecrystal/macros/scripts/events.asm
// ROM byte order: data operands come FIRST, strbuf LAST
static void encode_endgame(std::vector<uint8_t>& out, const CrystalCommandData& data) {
    if (auto* c = std::get_if<Cmd_Swarm>(&data)) {
        push_byte(out, CrystalOp::swarm); push_byte(out, c->flag); push_map_id(out, c->map);
    } else if (std::get_if<Cmd_Halloffame>(&data)) {
        push_byte(out, CrystalOp::halloffame);
    } else if (std::get_if<Cmd_Credits>(&data)) {
        push_byte(out, CrystalOp::credits);
    } else if (auto* c = std::get_if<Cmd_Warpfacing>(&data)) {
        push_byte(out, CrystalOp::warpfacing); push_byte(out, c->facing);
        push_map_id(out, c->map); push_byte(out, c->x); push_byte(out, c->y);
    } else if (auto* c = std::get_if<Cmd_Battletowertext>(&data)) {
        push_byte(out, CrystalOp::battletowertext); push_byte(out, c->bttext_id);
    } else if (auto* c = std::get_if<Cmd_Getlandmarkname>(&data)) {
        // ROM: db landmark_id, db strbuf
        push_byte(out, CrystalOp::getlandmarkname); push_byte(out, c->landmark_id); push_byte(out, c->strbuf);
    } else if (auto* c = std::get_if<Cmd_Gettrainerclassname>(&data)) {
        // ROM: db trainer_group, db strbuf
        push_byte(out, CrystalOp::gettrainerclassname); push_byte(out, c->trainer_group); push_byte(out, c->strbuf);
    } else if (auto* c = std::get_if<Cmd_Getname>(&data)) {
        // ROM: db type, db id, db strbuf
        push_byte(out, CrystalOp::getname); push_byte(out, c->type); push_byte(out, c->id); push_byte(out, c->strbuf);
    } else if (auto* c = std::get_if<Cmd_Wait>(&data)) {
        push_byte(out, CrystalOp::wait); push_byte(out, c->duration);
    } else if (std::get_if<Cmd_Checksave>(&data)) {
        push_byte(out, CrystalOp::checksave);
    } else if (auto* c = std::get_if<Cmd_Unknown>(&data)) {
        push_byte(out, c->opcode);
    }
}

std::vector<uint8_t> encode_crystal_command(const CrystalCommand& cmd) {
    std::vector<uint8_t> out;
    
    // Try each group in order - only one will produce output
    encode_control_flow(out, cmd.data);
    if (!out.empty()) return out;
    
    encode_asm_scene_vars(out, cmd.data);
    if (!out.empty()) return out;
    
    encode_items_phone_pokemon(out, cmd.data);
    if (!out.empty()) return out;
    
    encode_events_flags_warp(out, cmd.data);
    if (!out.empty()) return out;
    
    encode_string_itemnotify(out, cmd.data);
    if (!out.empty()) return out;
    
    encode_text(out, cmd.data);
    if (!out.empty()) return out;
    
    encode_display_battle(out, cmd.data);
    if (!out.empty()) return out;
    
    encode_movement(out, cmd.data);
    if (!out.empty()) return out;
    
    encode_effects_audio(out, cmd.data);
    if (!out.empty()) return out;
    
    encode_misc_control(out, cmd.data);
    if (!out.empty()) return out;
    
    encode_commerce(out, cmd.data);
    if (!out.empty()) return out;
    
    encode_endgame(out, cmd.data);
    
    return out;
}

bool validate_round_trip(const CrystalCommand& cmd) {
    auto encoded = encode_crystal_command(cmd);
    return encoded == cmd.span.raw_bytes;
}

} // namespace crystal

// File-scope explicit instantiation — paired with extern template in crystal_command.hpp.
// Must be at file scope so the enclosing namespace of std::vector is std ([temp.explicit]/7).
template class std::vector<crystal::CrystalCommand>;
