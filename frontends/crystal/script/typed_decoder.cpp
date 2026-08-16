// crystal/script/typed_decoder.cpp
// Stage 1 typed decoder implementation

#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/decoder.hpp"  // For CrystalOp constants
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace crystal {

TypedScriptDecoder::TypedScriptDecoder(const RomData& rom, const SymbolMap& symbols)
    : rom_(rom), symbols_(symbols) {}

uint8_t TypedScriptDecoder::peek_byte(TypedDecoderContext& ctx, size_t offset) const {
    return rom_.read_byte(ctx.pc + offset);
}

uint16_t TypedScriptDecoder::peek_word(TypedDecoderContext& ctx, size_t offset) const {
    return rom_.read_word(ctx.pc + offset);
}

uint8_t TypedScriptDecoder::read_byte(TypedDecoderContext& ctx, std::vector<uint8_t>& span) {
    uint8_t val = rom_.read_byte(ctx.pc++);
    span.push_back(val);
    return val;
}

uint16_t TypedScriptDecoder::read_word(TypedDecoderContext& ctx, std::vector<uint8_t>& span) {
    uint8_t lo = rom_.read_byte(ctx.pc++);
    uint8_t hi = rom_.read_byte(ctx.pc++);
    span.push_back(lo);
    span.push_back(hi);
    return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
}

CrystalMapId TypedScriptDecoder::read_map_id(TypedDecoderContext& ctx, std::vector<uint8_t>& span) {
    CrystalMapId m;
    m.group = read_byte(ctx, span);
    m.map = read_byte(ctx, span);
    return m;
}

uint32_t TypedScriptDecoder::resolve_local_pointer(TypedDecoderContext& ctx, uint16_t ptr) const {
    return rom_.bank_addr_to_flat(ctx.bank, ptr);
}

uint32_t TypedScriptDecoder::resolve_far_pointer(uint8_t bank, uint16_t ptr) const {
    return rom_.bank_addr_to_flat(bank, ptr);
}

CrystalLabelRef TypedScriptDecoder::make_label_ref(uint32_t address) {
    CrystalLabelRef ref;
    ref.rom_address = address;
    if (auto name = symbols_.name_at(address)) {
        ref.symbol_name = *name;
    } else {
        std::ostringstream ss;
        ss << "loc_" << std::hex << address;
        ref.symbol_name = ss.str();
    }
    return ref;
}

CrystalCommandData TypedScriptDecoder::dispatch_decode(uint8_t opcode, TypedDecoderContext& ctx,
                                                        std::vector<uint8_t>& span) {
    switch (opcode) {
        // Control flow (0x00-0x0D)
        case CrystalOp::scall: {
            Cmd_Scall cmd;
            cmd.pointer = read_word(ctx, span);
            cmd.target = make_label_ref(resolve_local_pointer(ctx, cmd.pointer));
            if (!ctx.visited.contains(cmd.target.rom_address)) {
                ctx.pending.push_back(cmd.target.rom_address);
            }
            return cmd;
        }
        case CrystalOp::farscall: {
            Cmd_Farscall cmd;
            cmd.bank = read_byte(ctx, span);
            cmd.pointer = read_word(ctx, span);
            cmd.target = make_label_ref(resolve_far_pointer(cmd.bank, cmd.pointer));
            if (!ctx.visited.contains(cmd.target.rom_address)) {
                ctx.pending.push_back(cmd.target.rom_address);
            }
            return cmd;
        }
        case CrystalOp::memcall: {
            Cmd_Memcall cmd;
            cmd.ram_address = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::sjump: {
            Cmd_Sjump cmd;
            cmd.pointer = read_word(ctx, span);
            cmd.target = make_label_ref(resolve_local_pointer(ctx, cmd.pointer));
            if (!ctx.visited.contains(cmd.target.rom_address)) {
                ctx.pending.push_back(cmd.target.rom_address);
            }
            return cmd;
        }
        case CrystalOp::farsjump: {
            Cmd_Farsjump cmd;
            cmd.bank = read_byte(ctx, span);
            cmd.pointer = read_word(ctx, span);
            cmd.target = make_label_ref(resolve_far_pointer(cmd.bank, cmd.pointer));
            if (!ctx.visited.contains(cmd.target.rom_address)) {
                ctx.pending.push_back(cmd.target.rom_address);
            }
            return cmd;
        }
        case CrystalOp::memjump: {
            Cmd_Memjump cmd;
            cmd.ram_address = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::ifequal: {
            Cmd_Ifequal cmd;
            cmd.value = read_byte(ctx, span);
            cmd.pointer = read_word(ctx, span);
            cmd.target = make_label_ref(resolve_local_pointer(ctx, cmd.pointer));
            if (!ctx.visited.contains(cmd.target.rom_address)) {
                ctx.pending.push_back(cmd.target.rom_address);
            }
            return cmd;
        }
        case CrystalOp::ifnotequal: {
            Cmd_Ifnotequal cmd;
            cmd.value = read_byte(ctx, span);
            cmd.pointer = read_word(ctx, span);
            cmd.target = make_label_ref(resolve_local_pointer(ctx, cmd.pointer));
            if (!ctx.visited.contains(cmd.target.rom_address)) {
                ctx.pending.push_back(cmd.target.rom_address);
            }
            return cmd;
        }
        case CrystalOp::iffalse: {
            Cmd_Iffalse cmd;
            cmd.pointer = read_word(ctx, span);
            cmd.target = make_label_ref(resolve_local_pointer(ctx, cmd.pointer));
            if (!ctx.visited.contains(cmd.target.rom_address)) {
                ctx.pending.push_back(cmd.target.rom_address);
            }
            return cmd;
        }
        case CrystalOp::iftrue: {
            Cmd_Iftrue cmd;
            cmd.pointer = read_word(ctx, span);
            cmd.target = make_label_ref(resolve_local_pointer(ctx, cmd.pointer));
            if (!ctx.visited.contains(cmd.target.rom_address)) {
                ctx.pending.push_back(cmd.target.rom_address);
            }
            return cmd;
        }
        case CrystalOp::ifgreater: {
            Cmd_Ifgreater cmd;
            cmd.value = read_byte(ctx, span);
            cmd.pointer = read_word(ctx, span);
            cmd.target = make_label_ref(resolve_local_pointer(ctx, cmd.pointer));
            if (!ctx.visited.contains(cmd.target.rom_address)) {
                ctx.pending.push_back(cmd.target.rom_address);
            }
            return cmd;
        }
        case CrystalOp::ifless: {
            Cmd_Ifless cmd;
            cmd.value = read_byte(ctx, span);
            cmd.pointer = read_word(ctx, span);
            cmd.target = make_label_ref(resolve_local_pointer(ctx, cmd.pointer));
            if (!ctx.visited.contains(cmd.target.rom_address)) {
                ctx.pending.push_back(cmd.target.rom_address);
            }
            return cmd;
        }
        case CrystalOp::jumpstd: {
            Cmd_Jumpstd cmd;
            cmd.std_id = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::callstd: {
            Cmd_Callstd cmd;
            cmd.std_id = read_word(ctx, span);
            return cmd;
        }
        
        // ASM/Special (0x0E-0x10)
        case CrystalOp::callasm: {
            Cmd_Callasm cmd;
            cmd.bank = read_byte(ctx, span);
            cmd.pointer = read_word(ctx, span);
            cmd.flat_address = resolve_far_pointer(cmd.bank, cmd.pointer);
            return cmd;
        }
        case CrystalOp::special: {
            Cmd_Special cmd;
            cmd.special_id = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::memcallasm: {
            Cmd_Memcallasm cmd;
            cmd.ram_address = read_word(ctx, span);
            return cmd;
        }
        // Map scene (0x11-0x14)
        case CrystalOp::checkmapscene: {
            Cmd_Checkmapscene cmd;
            cmd.map = read_map_id(ctx, span);
            return cmd;
        }
        case CrystalOp::setmapscene: {
            Cmd_Setmapscene cmd;
            cmd.map = read_map_id(ctx, span);
            cmd.scene = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::checkscene: {
            return Cmd_Checkscene{};
        }
        case CrystalOp::setscene: {
            Cmd_Setscene cmd;
            cmd.scene = read_byte(ctx, span);
            return cmd;
        }
        
        // Variables (0x15-0x1E)
        case CrystalOp::setval: {
            Cmd_Setval cmd;
            cmd.value = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::addval: {
            Cmd_Addval cmd;
            cmd.value = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::random: {
            Cmd_Random cmd;
            cmd.range = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::checkver: {
            return Cmd_Checkver{};
        }
        case CrystalOp::readmem: {
            Cmd_Readmem cmd;
            cmd.ram_address = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::writemem: {
            Cmd_Writemem cmd;
            cmd.ram_address = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::loadmem: {
            Cmd_Loadmem cmd;
            cmd.ram_address = read_word(ctx, span);
            cmd.value = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::readvar: {
            Cmd_Readvar cmd;
            cmd.var_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::writevar: {
            Cmd_Writevar cmd;
            cmd.var_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::loadvar: {
            Cmd_Loadvar cmd;
            cmd.var_id = read_byte(ctx, span);
            cmd.value = read_byte(ctx, span);
            return cmd;
        }
        // Items (0x1F-0x27)
        case CrystalOp::giveitem: {
            Cmd_Giveitem cmd;
            cmd.item = read_byte(ctx, span);
            cmd.quantity = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::takeitem: {
            Cmd_Takeitem cmd;
            cmd.item = read_byte(ctx, span);
            cmd.quantity = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::checkitem: {
            Cmd_Checkitem cmd;
            cmd.item = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::givemoney: {
            Cmd_Givemoney cmd;
            cmd.account = read_byte(ctx, span);
            cmd.money_byte1 = read_byte(ctx, span);
            cmd.money_byte2 = read_byte(ctx, span);
            cmd.money_byte3 = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::takemoney: {
            Cmd_Takemoney cmd;
            cmd.account = read_byte(ctx, span);
            cmd.money_byte1 = read_byte(ctx, span);
            cmd.money_byte2 = read_byte(ctx, span);
            cmd.money_byte3 = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::checkmoney: {
            Cmd_Checkmoney cmd;
            cmd.account = read_byte(ctx, span);
            cmd.money_byte1 = read_byte(ctx, span);
            cmd.money_byte2 = read_byte(ctx, span);
            cmd.money_byte3 = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::givecoins: {
            Cmd_Givecoins cmd;
            cmd.coins = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::takecoins: {
            Cmd_Takecoins cmd;
            cmd.coins = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::checkcoins: {
            Cmd_Checkcoins cmd;
            cmd.coins = read_word(ctx, span);
            return cmd;
        }
        // Phone (0x28-0x2A)
        case CrystalOp::addcellnum: {
            Cmd_Addcellnum cmd;
            cmd.person = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::delcellnum: {
            Cmd_Delcellnum cmd;
            cmd.person = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::checkcellnum: {
            Cmd_Checkcellnum cmd;
            cmd.person = read_byte(ctx, span);
            return cmd;
        }
        
        // Time/Pokemon (0x2B-0x30)
        case CrystalOp::checktime: {
            Cmd_Checktime cmd;
            cmd.time = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::checkpoke: {
            Cmd_Checkpoke cmd;
            cmd.pokemon = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::givepoke: {
            Cmd_Givepoke cmd;
            cmd.pokemon = read_byte(ctx, span);
            cmd.level = read_byte(ctx, span);
            cmd.item = read_byte(ctx, span);
            cmd.trainer = read_byte(ctx, span);
            cmd.has_extra_data = (cmd.trainer != 0);
            if (cmd.has_extra_data) {
                cmd.nickname_ptr = read_word(ctx, span);
                cmd.ot_name_ptr = read_word(ctx, span);
            } else {
                cmd.nickname_ptr = 0;
                cmd.ot_name_ptr = 0;
            }
            return cmd;
        }
        case CrystalOp::giveegg: {
            Cmd_Giveegg cmd;
            cmd.pokemon = read_byte(ctx, span);
            cmd.level = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::givepokemail: {
            Cmd_Givepokemail cmd;
            cmd.pointer = read_word(ctx, span);
            // Resolve local pointer to flat address using script's bank
            cmd.flat_address = resolve_local_pointer(ctx, cmd.pointer);
            return cmd;
        }
        case CrystalOp::checkpokemail: {
            Cmd_Checkpokemail cmd;
            cmd.pointer = read_word(ctx, span);
            // Resolve local pointer to flat address using script's bank
            cmd.flat_address = resolve_local_pointer(ctx, cmd.pointer);
            return cmd;
        }

        // Events/Flags (0x31-0x36)
        case CrystalOp::checkevent: {
            Cmd_Checkevent cmd;
            cmd.event_flag = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::clearevent: {
            Cmd_Clearevent cmd;
            cmd.event_flag = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::setevent: {
            Cmd_Setevent cmd;
            cmd.event_flag = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::checkflag: {
            Cmd_Checkflag cmd;
            cmd.engine_flag = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::clearflag: {
            Cmd_Clearflag cmd;
            cmd.engine_flag = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::setflag: {
            Cmd_Setflag cmd;
            cmd.engine_flag = read_word(ctx, span);
            return cmd;
        }
        
        // Wild encounters (0x37-0x38)
        case CrystalOp::wildon: {
            return Cmd_Wildon{};
        }
        case CrystalOp::wildoff: {
            return Cmd_Wildoff{};
        }
        
        // Map/Warp (0x39-0x3C)
        case CrystalOp::xycompare: {
            Cmd_Xycompare cmd;
            cmd.pointer = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::warpmod: {
            Cmd_Warpmod cmd;
            cmd.warp_id = read_byte(ctx, span);
            cmd.map = read_map_id(ctx, span);
            return cmd;
        }
        case CrystalOp::blackoutmod: {
            Cmd_Blackoutmod cmd;
            cmd.map = read_map_id(ctx, span);
            return cmd;
        }
        case CrystalOp::warp: {
            Cmd_Warp cmd;
            cmd.map = read_map_id(ctx, span);
            cmd.x = read_byte(ctx, span);
            cmd.y = read_byte(ctx, span);
            return cmd;
        }
        
        // String formatting (0x3D-0x44)
        case CrystalOp::getmoney: {
            Cmd_Getmoney cmd;
            cmd.strbuf = read_byte(ctx, span);
            cmd.account = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::getcoins: {
            Cmd_Getcoins cmd;
            cmd.strbuf = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::getnum: {
            Cmd_Getnum cmd;
            cmd.strbuf = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::getmonname: {
            Cmd_Getmonname cmd;
            cmd.strbuf = read_byte(ctx, span);
            cmd.pokemon = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::getitemname: {
            Cmd_Getitemname cmd;
            cmd.strbuf = read_byte(ctx, span);
            cmd.item = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::getcurlandmarkname: {
            Cmd_Getcurlandmarkname cmd;
            cmd.strbuf = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::gettrainername: {
            Cmd_Gettrainername cmd;
            cmd.strbuf = read_byte(ctx, span);
            cmd.trainer_group = read_byte(ctx, span);
            cmd.trainer_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::getstring: {
            Cmd_Getstring cmd;
            cmd.strbuf = read_byte(ctx, span);
            cmd.text_pointer = read_word(ctx, span);
            return cmd;
        }
        
        // Item notify (0x45-0x46)
        case CrystalOp::itemnotify: {
            return Cmd_Itemnotify{};
        }
        case CrystalOp::pocketisfull: {
            return Cmd_Pocketisfull{};
        }
        
        // Text (0x47-0x55)
        case CrystalOp::opentext: {
            return Cmd_Opentext{};
        }
        case CrystalOp::reanchormap: {
            Cmd_Reanchormap cmd;
            cmd.dummy = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::closetext: {
            return Cmd_Closetext{};
        }
        case CrystalOp::writeunusedbyte: {
            Cmd_Writeunusedbyte cmd;
            cmd.byte = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::farwritetext: {
            Cmd_Farwritetext cmd;
            cmd.bank = read_byte(ctx, span);
            cmd.pointer = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::writetext: {
            Cmd_Writetext cmd;
            cmd.text_pointer = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::repeattext: {
            Cmd_Repeattext cmd;
            cmd.byte1 = read_byte(ctx, span);
            cmd.byte2 = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::yesorno: {
            return Cmd_Yesorno{};
        }
        case CrystalOp::loadmenu: {
            Cmd_Loadmenu cmd;
            cmd.menu_header = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::closewindow: {
            return Cmd_Closewindow{};
        }
        case CrystalOp::jumptextfaceplayer: {
            Cmd_Jumptextfaceplayer cmd;
            cmd.text_pointer = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::farjumptext: {
            Cmd_Farjumptext cmd;
            cmd.bank = read_byte(ctx, span);
            cmd.pointer = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::jumptext: {
            Cmd_Jumptext cmd;
            cmd.text_pointer = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::waitbutton: {
            return Cmd_Waitbutton{};
        }
        case CrystalOp::promptbutton: {
            return Cmd_Promptbutton{};
        }
        
        // Pokemon display (0x56-0x5B)
        case CrystalOp::pokepic: {
            Cmd_Pokepic cmd;
            cmd.pokemon = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::closepokepic: {
            return Cmd_Closepokepic{};
        }
        case CrystalOp::_2dmenu: {
            return Cmd_2dmenu{};
        }
        case CrystalOp::verticalmenu: {
            return Cmd_Verticalmenu{};
        }
        case CrystalOp::loadpikachudata: {
            return Cmd_Loadpikachudata{};
        }
        case CrystalOp::randomwildmon: {
            return Cmd_Randomwildmon{};
        }
        
        // Battle setup (0x5C-0x67)
        case CrystalOp::loadtemptrainer: {
            return Cmd_Loadtemptrainer{};
        }
        case CrystalOp::loadwildmon: {
            Cmd_Loadwildmon cmd;
            cmd.pokemon = read_byte(ctx, span);
            cmd.level = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::loadtrainer: {
            Cmd_Loadtrainer cmd;
            cmd.trainer_group = read_byte(ctx, span);
            cmd.trainer_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::startbattle: {
            return Cmd_Startbattle{};
        }
        case CrystalOp::reloadmapafterbattle: {
            return Cmd_Reloadmapafterbattle{};
        }
        case CrystalOp::catchtutorial: {
            Cmd_Catchtutorial cmd;
            cmd.byte = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::trainertext: {
            Cmd_Trainertext cmd;
            cmd.text_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::trainerflagaction: {
            Cmd_Trainerflagaction cmd;
            cmd.action = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::winlosstext: {
            Cmd_Winlosstext cmd;
            cmd.win_text = read_word(ctx, span);
            cmd.loss_text = read_word(ctx, span);
            // Resolve local pointers to flat addresses using script's bank
            // 0 = no text (Crystal uses "winlosstext WinText, 0" for no loss text)
            cmd.win_text_address = (cmd.win_text != 0) 
                ? resolve_local_pointer(ctx, cmd.win_text) : 0;
            cmd.loss_text_address = (cmd.loss_text != 0)
                ? resolve_local_pointer(ctx, cmd.loss_text) : 0;
            return cmd;
        }
        case CrystalOp::scripttalkafter: {
            return Cmd_Scripttalkafter{};
        }
        case CrystalOp::endifjustbattled: {
            return Cmd_Endifjustbattled{};
        }
        case CrystalOp::checkjustbattled: {
            return Cmd_Checkjustbattled{};
        }
        
        // Movement (0x68-0x77)
        case CrystalOp::setlasttalked: {
            Cmd_Setlasttalked cmd;
            cmd.object_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::applymovement: {
            Cmd_Applymovement cmd;
            cmd.object_id = read_byte(ctx, span);
            cmd.movement_pointer = read_word(ctx, span);
            // Decode movement data from ROM
            uint32_t mov_addr = resolve_local_pointer(ctx, cmd.movement_pointer);
            cmd.raw_movements = decode_movement_data(mov_addr);
            cmd.commands = parse_movement_commands(cmd.raw_movements);
            return cmd;
        }
        case CrystalOp::applymovementlasttalked: {
            Cmd_Applymovementlasttalked cmd;
            cmd.movement_pointer = read_word(ctx, span);
            // Decode movement data from ROM
            uint32_t mov_addr = resolve_local_pointer(ctx, cmd.movement_pointer);
            cmd.raw_movements = decode_movement_data(mov_addr);
            cmd.commands = parse_movement_commands(cmd.raw_movements);
            return cmd;
        }
        case CrystalOp::faceplayer: {
            return Cmd_Faceplayer{};
        }
        case CrystalOp::faceobject: {
            Cmd_Faceobject cmd;
            cmd.object1 = read_byte(ctx, span);
            cmd.object2 = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::variablesprite: {
            Cmd_Variablesprite cmd;
            cmd.slot = read_byte(ctx, span);
            cmd.sprite = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::disappear: {
            Cmd_Disappear cmd;
            cmd.object_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::appear: {
            Cmd_Appear cmd;
            cmd.object_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::follow: {
            Cmd_Follow cmd;
            cmd.object2 = read_byte(ctx, span);
            cmd.object1 = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::stopfollow: {
            return Cmd_Stopfollow{};
        }
        case CrystalOp::moveobject: {
            Cmd_Moveobject cmd;
            cmd.object_id = read_byte(ctx, span);
            cmd.x = read_byte(ctx, span);
            cmd.y = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::writeobjectxy: {
            Cmd_Writeobjectxy cmd;
            cmd.object_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::loademote: {
            Cmd_Loademote cmd;
            cmd.bubble = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::showemote: {
            Cmd_Showemote cmd;
            cmd.bubble = read_byte(ctx, span);
            cmd.object_id = read_byte(ctx, span);
            cmd.time = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::turnobject: {
            Cmd_Turnobject cmd;
            cmd.object_id = read_byte(ctx, span);
            cmd.facing = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::follownotexact: {
            Cmd_Follownotexact cmd;
            cmd.object2 = read_byte(ctx, span);
            cmd.object1 = read_byte(ctx, span);
            return cmd;
        }
        
        // Effects (0x78-0x7E)
        case CrystalOp::earthquake: {
            Cmd_Earthquake cmd;
            cmd.param = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::changemapblocks: {
            Cmd_Changemapblocks cmd;
            cmd.bank = read_byte(ctx, span);
            cmd.pointer = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::changeblock: {
            Cmd_Changeblock cmd;
            cmd.x = read_byte(ctx, span);
            cmd.y = read_byte(ctx, span);
            cmd.block = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::reloadmap: {
            return Cmd_Reloadmap{};
        }
        case CrystalOp::refreshmap: {
            return Cmd_Refreshmap{};
        }
        case CrystalOp::writecmdqueue: {
            Cmd_Writecmdqueue cmd;
            cmd.queue_pointer = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::delcmdqueue: {
            Cmd_Delcmdqueue cmd;
            cmd.byte = read_byte(ctx, span);
            return cmd;
        }
        
        // Audio (0x7F-0x88)
        case CrystalOp::playmusic: {
            Cmd_Playmusic cmd;
            cmd.music = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::encountermusic: {
            return Cmd_Encountermusic{};
        }
        case CrystalOp::musicfadeout: {
            Cmd_Musicfadeout cmd;
            cmd.music = read_word(ctx, span);
            cmd.fadetime = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::playmapmusic: {
            return Cmd_Playmapmusic{};
        }
        case CrystalOp::dontrestartmapmusic: {
            return Cmd_Dontrestartmapmusic{};
        }
        case CrystalOp::cry: {
            Cmd_Cry cmd;
            cmd.cry_id = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::playsound: {
            Cmd_Playsound cmd;
            cmd.sound = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::waitsfx: {
            return Cmd_Waitsfx{};
        }
        case CrystalOp::warpsound: {
            return Cmd_Warpsound{};
        }
        case CrystalOp::specialsound: {
            return Cmd_Specialsound{};
        }
        
        // Misc control (0x89-0x93)
        case CrystalOp::autoinput: {
            Cmd_Autoinput cmd;
            cmd.bank = read_byte(ctx, span);
            cmd.pointer = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::newloadmap: {
            Cmd_Newloadmap cmd;
            cmd.method = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::pause: {
            Cmd_Pause cmd;
            cmd.length = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::deactivatefacing: {
            Cmd_Deactivatefacing cmd;
            cmd.time = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::sdefer: {
            Cmd_Sdefer cmd;
            cmd.pointer = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::warpcheck: {
            return Cmd_Warpcheck{};
        }
        case CrystalOp::stopandsjump: {
            Cmd_Stopandsjump cmd;
            cmd.pointer = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::endcallback: {
            return Cmd_Endcallback{};
        }
        case CrystalOp::end: {
            return Cmd_End{};
        }
        case CrystalOp::reloadend: {
            Cmd_Reloadend cmd;
            cmd.method = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::endall: {
            return Cmd_Endall{};
        }
        
        // Commerce (0x94-0x9D)
        case CrystalOp::pokemart: {
            Cmd_Pokemart cmd;
            cmd.dialog_id = read_byte(ctx, span);
            cmd.mart_id = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::elevator: {
            Cmd_Elevator cmd;
            uint16_t ptr = read_word(ctx, span);
            cmd.floor_list = ptr;  // Keep raw pointer for round-trip encoding
            // Resolve pointer to flat address using script's bank
            cmd.floor_list_address = resolve_local_pointer(ctx, ptr);
            return cmd;
        }
        case CrystalOp::trade: {
            Cmd_Trade cmd;
            cmd.trade_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::askforphonenumber: {
            Cmd_Askforphonenumber cmd;
            cmd.number = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::phonecall: {
            Cmd_Phonecall cmd;
            cmd.caller_name = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::hangup: {
            return Cmd_Hangup{};
        }
        case CrystalOp::describedecoration: {
            Cmd_Describedecoration cmd;
            cmd.byte = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::fruittree: {
            Cmd_Fruittree cmd;
            cmd.tree_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::specialphonecall: {
            Cmd_Specialphonecall cmd;
            cmd.call_id = read_word(ctx, span);
            return cmd;
        }
        case CrystalOp::checkphonecall: {
            return Cmd_Checkphonecall{};
        }
        
        // Verbose items (0x9E-0x9F)
        case CrystalOp::verbosegiveitem: {
            Cmd_Verbosegiveitem cmd;
            cmd.item = read_byte(ctx, span);
            cmd.quantity = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::verbosegiveitemvar: {
            Cmd_Verbosegiveitemvar cmd;
            cmd.item = read_byte(ctx, span);
            cmd.var = read_byte(ctx, span);
            return cmd;
        }
        
        // End game (0xA0-0xA9)
        case CrystalOp::swarm: {
            Cmd_Swarm cmd;
            cmd.flag = read_byte(ctx, span);
            cmd.map = read_map_id(ctx, span);
            return cmd;
        }
        case CrystalOp::halloffame: {
            return Cmd_Halloffame{};
        }
        case CrystalOp::credits: {
            return Cmd_Credits{};
        }
        case CrystalOp::warpfacing: {
            Cmd_Warpfacing cmd;
            cmd.facing = read_byte(ctx, span);
            cmd.map = read_map_id(ctx, span);
            cmd.x = read_byte(ctx, span);
            cmd.y = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::battletowertext: {
            Cmd_Battletowertext cmd;
            cmd.bttext_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::getlandmarkname: {
            Cmd_Getlandmarkname cmd;
            cmd.strbuf = read_byte(ctx, span);
            cmd.landmark_id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::gettrainerclassname: {
            Cmd_Gettrainerclassname cmd;
            cmd.strbuf = read_byte(ctx, span);
            cmd.trainer_group = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::getname: {
            Cmd_Getname cmd;
            cmd.strbuf = read_byte(ctx, span);
            cmd.type = read_byte(ctx, span);
            cmd.id = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::wait: {
            Cmd_Wait cmd;
            cmd.duration = read_byte(ctx, span);
            return cmd;
        }
        case CrystalOp::checksave: {
            return Cmd_Checksave{};
        }
        
        // Unknown opcode (>= 0xAA)
        default: {
            Cmd_Unknown cmd;
            cmd.opcode = opcode;
            return cmd;
        }
    }
}

CrystalCommand TypedScriptDecoder::decode_command(TypedDecoderContext& ctx) {
    CrystalCommand cmd;
    
    uint32_t start_address = ctx.pc;
    std::vector<uint8_t> span_bytes;
    
    // Read opcode
    uint8_t opcode = read_byte(ctx, span_bytes);
    
    // Dispatch to decode the command data
    cmd.data = dispatch_decode(opcode, ctx, span_bytes);
    
    // Fill in span
    cmd.span.rom_address = start_address;
    cmd.span.raw_bytes = std::move(span_bytes);
    
    // Set status - Cmd_Unknown is Malformed, all others are Unlowered (not yet semantically lowered)
    if (std::holds_alternative<Cmd_Unknown>(cmd.data)) {
        cmd.status = DecodeStatus::Malformed;
    } else {
        cmd.status = DecodeStatus::Unlowered;
    }
    
    return cmd;
}

CrystalScriptIR TypedScriptDecoder::decode_script(uint32_t address, const std::string& name) {
    CrystalScriptIR ir;
    ir.name = name.empty() ? make_label_ref(address).symbol_name : name;
    ir.entry_address = address;
    
    TypedDecoderContext ctx;
    ctx.pc = address;
    ctx.bank = rom_.flat_to_bank(address);
    ctx.pending.push_back(address);
    
    while (!ctx.pending.empty()) {
        uint32_t current = ctx.pending.back();
        ctx.pending.pop_back();
        
        // Skip if already visited as a block entry
        if (ctx.visited.contains(current)) {
            continue;
        }
        
        // If this address was already decoded as part of another block's
        // sequential decoding, it's still valid as a branch target.
        // Mark visited but don't re-decode.
        if (ctx.decoded_commands.contains(current)) {
            ctx.visited.insert(current);
            continue;
        }
        
        ctx.pc = current;
        ctx.bank = rom_.flat_to_bank(current);
        ctx.visited.insert(current);
        
        // Decode commands until terminator or already-decoded address
        while (true) {
            // Check if we've reached an already-decoded command address
            // This happens when a branch target lands in the middle of a
            // previously-decoded sequence. Stop here - CFG will handle the split.
            if (ctx.decoded_commands.contains(ctx.pc)) {
                // We've hit a command that was already decoded.
                // This is a valid back-edge or join point.
                // Don't re-decode; the CFG builder will identify this as a leader.
                break;
            }
            
            uint32_t cmd_addr = ctx.pc;  // Capture before decode_command advances pc
            CrystalCommand cmd = decode_command(ctx);
            
            // Record this command's address for uniqueness tracking
            size_t cmd_index = ir.commands.size();
            ctx.decoded_commands[cmd_addr] = cmd_index;
            
            ir.commands.push_back(cmd);
            
            stats_.instructions_decoded++;
            stats_.opcode_counts[cmd.opcode()]++;
            
            if (std::holds_alternative<Cmd_Unknown>(cmd.data)) {
                stats_.unknown_opcodes++;
            }
            
            // Check if this is a terminator
            if (cmd.is_terminator()) {
                break;
            }
        }
    }
    
    stats_.scripts_decoded++;
    return ir;
}

TypedDecoderStats TypedScriptDecoder::get_stats() const {
    TypedDecoderStats stats;
    stats.scripts_decoded = stats_.scripts_decoded;
    stats.commands_decoded = stats_.instructions_decoded;
    stats.unknown_opcodes = stats_.unknown_opcodes;
    stats.opcode_counts = stats_.opcode_counts;
    return stats;
}

bool TypedScriptDecoder::validate_round_trip(const CrystalCommand& cmd) {
    auto encoded = encode_crystal_command(cmd);
    return encoded == cmd.span.raw_bytes;
}

bool TypedScriptDecoder::validate_script_round_trip(const CrystalScriptIR& ir, std::vector<std::string>* errors) {
    bool all_passed = true;
    
    for (size_t i = 0; i < ir.commands.size(); ++i) {
        const auto& cmd = ir.commands[i];
        if (!validate_round_trip(cmd)) {
            all_passed = false;
            if (errors) {
                std::ostringstream ss;
                ss << "Command " << i << " at 0x" << std::hex << cmd.span.rom_address
                   << " (opcode 0x" << static_cast<int>(cmd.opcode()) << "): round-trip mismatch";
                
                // Show original bytes
                ss << "\n  Original: ";
                for (uint8_t b : cmd.span.raw_bytes) {
                    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b) << " ";
                }
                
                // Show encoded bytes
                auto encoded = encode_crystal_command(cmd);
                ss << "\n  Encoded:  ";
                for (uint8_t b : encoded) {
                    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b) << " ";
                }
                
                errors->push_back(ss.str());
            }
        }
    }
    
    return all_passed;
}

// =============================================================================
// MOVEMENT DATA DECODING
// =============================================================================

std::vector<uint8_t> TypedScriptDecoder::decode_movement_data(uint32_t address) const {
    std::vector<uint8_t> movements;
    uint32_t pos = address;
    
    while (true) {
        uint8_t cmd = rom_.read_byte(pos++);
        movements.push_back(cmd);
        
        // Movement terminators from pokecrystal/macros/scripts/movement.asm:
        // step_end = 0x47, step_wait_end = 0x48, remove_object = 0x49, 
        // step_stop = 0x4B, step_loop = 0x4A
        if (cmd == 0x47 || cmd == 0x48 || cmd == 0x49 || cmd == 0x4A || cmd == 0x4B) break;
        
        // Some commands have additional parameter bytes
        // step_sleep with param (0x46) reads one more byte
        if (cmd == 0x46) {
            movements.push_back(rom_.read_byte(pos++));
        }
        // step_dig (0x4F), step_shake (0x55), rock_smash (0x57)
        // also read one param byte
        if (cmd == 0x4F || cmd == 0x55 || cmd == 0x57) {
            movements.push_back(rom_.read_byte(pos++));
        }
        
        // Safety limit
        if (movements.size() > 256) break;
    }
    
    return movements;
}

// Parse raw movement bytes into semantic MovementCommand array
// AUTHORITATIVE SOURCE: pokecrystal/macros/scripts/movement.asm
std::vector<enginemon::MovementCommand> TypedScriptDecoder::parse_movement_commands(
    const std::vector<uint8_t>& raw) const {
    
    using enginemon::MovementCommand;
    using enginemon::MovementType;
    using enginemon::Direction;
    
    std::vector<MovementCommand> commands;
    
    for (size_t i = 0; i < raw.size(); ++i) {
        uint8_t byte = raw[i];
        MovementCommand cmd;
        cmd.param = 0;
        cmd.direction = Direction::Down;
        
        // Directional commands (0x00-0x37) are base + direction
        // direction = byte & 0x03, type = byte >> 2
        if (byte < 0x38) {
            uint8_t dir = byte & 0x03;
            uint8_t type = byte >> 2;
            
            cmd.direction = static_cast<Direction>(dir);
            cmd.type = static_cast<MovementType>(type);
        }
        // Control commands (0x38-0x3D)
        else if (byte >= 0x38 && byte <= 0x3D) {
            cmd.type = static_cast<MovementType>(byte);
        }
        // step_sleep 1-8 (0x3E-0x45)
        else if (byte >= 0x3E && byte <= 0x45) {
            cmd.type = MovementType::StepSleep;
            cmd.param = byte - 0x3E + 1;  // 1-8 frames
        }
        // step_sleep with extended param (0x46)
        else if (byte == 0x46) {
            cmd.type = MovementType::StepSleep;
            if (i + 1 < raw.size()) {
                cmd.param = raw[++i];
            }
        }
        // step_end (0x47)
        else if (byte == 0x47) {
            cmd.type = MovementType::StepEnd;
        }
        // step_wait_end (0x48)
        else if (byte == 0x48) {
            cmd.type = MovementType::StepWaitEnd;
        }
        // remove_object (0x49)
        else if (byte == 0x49) {
            cmd.type = MovementType::RemoveObject;
        }
        // step_loop (0x4A)
        else if (byte == 0x4A) {
            cmd.type = MovementType::StepLoop;
        }
        // step_stop (0x4B)
        else if (byte == 0x4B) {
            cmd.type = MovementType::StepStop;
        }
        // teleport_from (0x4C)
        else if (byte == 0x4C) {
            cmd.type = MovementType::TeleportFrom;
        }
        // teleport_to (0x4D)
        else if (byte == 0x4D) {
            cmd.type = MovementType::TeleportTo;
        }
        // skyfall (0x4E)
        else if (byte == 0x4E) {
            cmd.type = MovementType::Skyfall;
        }
        // step_dig (0x4F)
        else if (byte == 0x4F) {
            cmd.type = MovementType::StepDig;
            if (i + 1 < raw.size()) {
                cmd.param = raw[++i];
            }
        }
        // step_bump (0x50)
        else if (byte == 0x50) {
            cmd.type = MovementType::StepBump;
        }
        // step_shake (0x55)
        else if (byte == 0x55) {
            cmd.type = MovementType::StepShake;
            if (i + 1 < raw.size()) {
                cmd.param = raw[++i];
            }
        }
        // tree_shake (0x56)
        else if (byte == 0x56) {
            cmd.type = MovementType::TreeShake;
        }
        // rock_smash (0x57)
        else if (byte == 0x57) {
            cmd.type = MovementType::RockSmash;
            if (i + 1 < raw.size()) {
                cmd.param = raw[++i];
            }
        }
        // Unknown - treat as step_end for safety
        else {
            cmd.type = MovementType::StepEnd;
        }
        
        commands.push_back(cmd);
    }
    
    return commands;
}

} // namespace crystal
