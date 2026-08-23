#pragma once
// engine/world/pokemon_icons.hpp
// Pokémon species → overworld icon type name mapping.
//
// Source: pokecrystal/data/pokemon/menu_icons.asm (MonMenuIcons table)
// Maps Crystal species index (1-251) to the semantic icon type name used as
// the pokemon_icon:<name> package key.
//
// This is engine-layer knowledge — it encodes the Crystal MonMenuIcons table
// without importing any Crystal frontend headers.
//
// Usage:
//   std::string icon = species_to_icon_type_name(25);  // "pikachu"
//   std::string sprite_id = "pokemon_icon:" + icon;    // "pokemon_icon:pikachu"

#include "engine/core/types.hpp"
#include <string>
#include <string_view>

namespace enginemon {

// Returns the icon type name for a species (1-251), or "" for invalid/0.
// Source: pokecrystal/data/pokemon/menu_icons.asm MonMenuIcons table.
inline std::string_view species_to_icon_type_name(SpeciesId species) {
    if (species == 0 || species > 251) return "";

    // MonMenuIcons: 251 entries, 1-indexed by species constant
    // Source: pokecrystal/data/pokemon/menu_icons.asm
    static constexpr const char* ICONS[252] = {
        nullptr,          // 0 = no species
        "bulbasaur",      //   1 BULBASAUR
        "bulbasaur",      //   2 IVYSAUR
        "bulbasaur",      //   3 VENUSAUR
        "charmander",     //   4 CHARMANDER
        "charmander",     //   5 CHARMELEON
        "bigmon",         //   6 CHARIZARD
        "squirtle",       //   7 SQUIRTLE
        "squirtle",       //   8 WARTORTLE
        "squirtle",       //   9 BLASTOISE
        "caterpillar",    //  10 CATERPIE
        "caterpillar",    //  11 METAPOD
        "moth",           //  12 BUTTERFREE
        "caterpillar",    //  13 WEEDLE
        "caterpillar",    //  14 KAKUNA
        "bug",            //  15 BEEDRILL
        "bird",           //  16 PIDGEY
        "bird",           //  17 PIDGEOTTO
        "bird",           //  18 PIDGEOT
        "fox",            //  19 RATTATA
        "fox",            //  20 RATICATE
        "bird",           //  21 SPEAROW
        "bird",           //  22 FEAROW
        "serpent",        //  23 EKANS
        "serpent",        //  24 ARBOK
        "pikachu",        //  25 PIKACHU
        "pikachu",        //  26 RAICHU
        "shell",          //  27 SANDSHREW
        "shell",          //  28 SANDSLASH
        "clefairy",       //  29 NIDORAN_F
        "clefairy",       //  30 NIDORINA
        "clefairy",       //  31 NIDOQUEEN
        "clefairy",       //  32 NIDORAN_M
        "clefairy",       //  33 NIDORINO
        "monster",        //  34 NIDOKING
        "clefairy",       //  35 CLEFAIRY
        "clefairy",       //  36 CLEFABLE
        "fox",            //  37 VULPIX
        "fox",            //  38 NINETALES
        "jigglypuff",     //  39 JIGGLYPUFF
        "jigglypuff",     //  40 WIGGLYTUFF
        "bat",            //  41 ZUBAT
        "bat",            //  42 GOLBAT
        "oddish",         //  43 ODDISH
        "oddish",         //  44 GLOOM
        "oddish",         //  45 VILEPLUME
        "bug",            //  46 PARAS
        "bug",            //  47 PARASECT
        "bug",            //  48 VENONAT
        "moth",           //  49 VENOMOTH
        "diglett",        //  50 DIGLETT
        "diglett",        //  51 DUGTRIO
        "fox",            //  52 MEOWTH
        "fox",            //  53 PERSIAN
        "fish",           //  54 PSYDUCK
        "fish",           //  55 GOLDUCK
        "fighter",        //  56 MANKEY
        "fighter",        //  57 PRIMEAPE
        "fox",            //  58 GROWLITHE
        "fox",            //  59 ARCANINE
        "poliwag",        //  60 POLIWAG
        "poliwag",        //  61 POLIWHIRL
        "poliwag",        //  62 POLIWRATH
        "humanshape",     //  63 ABRA
        "humanshape",     //  64 KADABRA
        "humanshape",     //  65 ALAKAZAM
        "fighter",        //  66 MACHOP
        "fighter",        //  67 MACHOKE
        "fighter",        //  68 MACHAMP
        "oddish",         //  69 BELLSPROUT
        "oddish",         //  70 WEEPINBELL
        "oddish",         //  71 VICTREEBEL
        "jellyfish",      //  72 TENTACOOL
        "jellyfish",      //  73 TENTACRUEL
        "geodude",        //  74 GEODUDE
        "geodude",        //  75 GRAVELER
        "geodude",        //  76 GOLEM
        "equine",         //  77 PONYTA
        "equine",         //  78 RAPIDASH
        "slowpoke",       //  79 SLOWPOKE
        "slowpoke",       //  80 SLOWBRO
        "voltorb",        //  81 MAGNEMITE
        "voltorb",        //  82 MAGNETON
        "bird",           //  83 FARFETCH_D
        "bird",           //  84 DODUO
        "bird",           //  85 DODRIO
        "lapras",         //  86 SEEL
        "lapras",         //  87 DEWGONG
        "blob",           //  88 GRIMER
        "blob",           //  89 MUK
        "shell",          //  90 SHELLDER
        "shell",          //  91 CLOYSTER
        "ghost",          //  92 GASTLY
        "ghost",          //  93 HAUNTER
        "ghost",          //  94 GENGAR
        "serpent",        //  95 ONIX
        "humanshape",     //  96 DROWZEE
        "humanshape",     //  97 HYPNO
        "shell",          //  98 KRABBY
        "shell",          //  99 KINGLER
        "voltorb",        // 100 VOLTORB
        "voltorb",        // 101 ELECTRODE
        "oddish",         // 102 EXEGGCUTE
        "oddish",         // 103 EXEGGUTOR
        "monster",        // 104 CUBONE
        "monster",        // 105 MAROWAK
        "humanshape",     // 106 HITMONLEE
        "humanshape",     // 107 HITMONCHAN
        "serpent",        // 108 LICKITUNG
        "serpent",        // 109 KOFFING
        "blob",           // 110 WEEZING
        "equine",         // 111 RHYHORN
        "monster",        // 112 RHYDON
        "clefairy",       // 113 CHANSEY
        "oddish",         // 114 TANGELA
        "monster",        // 115 KANGASKHAN
        "fish",           // 116 HORSEA
        "fish",           // 117 SEADRA
        "fish",           // 118 GOLDEEN
        "fish",           // 119 SEAKING
        "staryu",         // 120 STARYU
        "staryu",         // 121 STARMIE
        "humanshape",     // 122 MR_MIME
        "bug",            // 123 SCYTHER
        "humanshape",     // 124 JYNX
        "voltorb",        // 125 ELECTABUZZ
        "humanshape",     // 126 MAGMAR
        "bug",            // 127 PINSIR
        "equine",         // 128 TAUROS
        "fish",           // 129 MAGIKARP
        "gyarados",       // 130 GYARADOS
        "lapras",         // 131 LAPRAS
        "blob",           // 132 DITTO
        "fox",            // 133 EEVEE
        "fox",            // 134 VAPOREON
        "fox",            // 135 JOLTEON
        "fox",            // 136 FLAREON
        "voltorb",        // 137 PORYGON
        "shell",          // 138 OMANYTE
        "shell",          // 139 OMASTAR
        "shell",          // 140 KABUTO
        "shell",          // 141 KABUTOPS
        "bird",           // 142 AERODACTYL
        "snorlax",        // 143 SNORLAX
        "bird",           // 144 ARTICUNO
        "bird",           // 145 ZAPDOS
        "bird",           // 146 MOLTRES
        "serpent",        // 147 DRATINI
        "serpent",        // 148 DRAGONAIR
        "bigmon",         // 149 DRAGONITE
        "humanshape",     // 150 MEWTWO
        "humanshape",     // 151 MEW
        "oddish",         // 152 CHIKORITA
        "oddish",         // 153 BAYLEEF
        "oddish",         // 154 MEGANIUM
        "fox",            // 155 CYNDAQUIL
        "fox",            // 156 QUILAVA
        "bigmon",         // 157 TYPHLOSION
        "serpent",        // 158 TOTODILE
        "serpent",        // 159 CROCONAW
        "monster",        // 160 FERALIGATR
        "bird",           // 161 SENTRET
        "bird",           // 162 FURRET
        "bird",           // 163 HOOTHOOT
        "bird",           // 164 NOCTOWL
        "bug",            // 165 LEDYBA
        "bug",            // 166 LEDIAN
        "bug",            // 167 SPINARAK
        "bug",            // 168 ARIADOS
        "bat",            // 169 CROBAT
        "jellyfish",      // 170 CHINCHOU
        "jellyfish",      // 171 LANTURN
        "pikachu",        // 172 PICHU
        "clefairy",       // 173 CLEFFA
        "jigglypuff",     // 174 IGGLYBUFF
        "clefairy",       // 175 TOGEPI
        "bird",           // 176 TOGETIC
        "bird",           // 177 NATU
        "bird",           // 178 XATU
        "equine",         // 179 MAREEP
        "monster",        // 180 FLAAFFY
        "monster",        // 181 AMPHAROS
        "oddish",         // 182 BELLOSSOM
        "jigglypuff",     // 183 MARILL
        "jigglypuff",     // 184 AZUMARILL
        "sudowoodo",      // 185 SUDOWOODO
        "poliwag",        // 186 POLITOED
        "oddish",         // 187 HOPPIP
        "oddish",         // 188 SKIPLOOM
        "oddish",         // 189 JUMPLUFF
        "fox",            // 190 AIPOM
        "oddish",         // 191 SUNKERN
        "oddish",         // 192 SUNFLORA
        "bug",            // 193 YANMA
        "fish",           // 194 WOOPER
        "fish",           // 195 QUAGSIRE
        "fox",            // 196 ESPEON
        "fox",            // 197 UMBREON
        "bird",           // 198 MURKROW
        "slowpoke",       // 199 SLOWKING
        "ghost",          // 200 MISDREAVUS
        "unown",          // 201 UNOWN
        "humanshape",     // 202 WOBBUFFET
        "bird",           // 203 GIRAFARIG
        "bug",            // 204 PINECO
        "bug",            // 205 FORRETRESS
        "serpent",        // 206 DUNSPARCE
        "bat",            // 207 GLIGAR
        "serpent",        // 208 STEELIX
        "fox",            // 209 SNUBBULL
        "fox",            // 210 GRANBULL
        "fish",           // 211 QWILFISH
        "bug",            // 212 SCIZOR
        "bug",            // 213 SHUCKLE
        "monster",        // 214 HERACROSS
        "fox",            // 215 SNEASEL
        "equine",         // 216 TEDDIURSA
        "equine",         // 217 URSARING
        "blob",           // 218 SLUGMA
        "blob",           // 219 MAGCARGO
        "equine",         // 220 SWINUB
        "equine",         // 221 PILOSWINE
        "fish",           // 222 CORSOLA
        "fish",           // 223 REMORAID
        "fish",           // 224 OCTILLERY
        "bird",           // 225 DELIBIRD
        "bigmon",         // 226 MANTINE
        "bird",           // 227 SKARMORY
        "fox",            // 228 HOUNDOUR
        "fox",            // 229 HOUNDOOM
        "fish",           // 230 KINGDRA
        "equine",         // 231 PHANPY
        "equine",         // 232 DONPHAN
        "voltorb",        // 233 PORYGON2
        "equine",         // 234 STANTLER
        "humanshape",     // 235 SMEARGLE
        "humanshape",     // 236 TYROGUE
        "humanshape",     // 237 HITMONTOP
        "humanshape",     // 238 SMOOCHUM
        "humanshape",     // 239 ELEKID
        "humanshape",     // 240 MAGBY
        "equine",         // 241 MILTANK
        "clefairy",       // 242 BLISSEY
        "bird",           // 243 RAIKOU
        "bird",           // 244 ENTEI
        "bird",           // 245 SUICUNE
        "serpent",        // 246 LARVITAR
        "serpent",        // 247 PUPITAR
        "bigmon",         // 248 TYRANITAR
        "lugia",          // 249 LUGIA
        "ho_oh",          // 250 HO_OH
        "humanshape",     // 251 CELEBI
    };

    const char* name = ICONS[species];
    return name ? name : "";
}

// Resolve a "daycare:N" sprite_id to the "pokemon_icon:<name>" sprite_id
// using the current Day Care occupancy from GameState.
// Returns "" if the slot is empty (species = 0).
// Returns "pokemon_icon:<icon_type_name>" if occupied.
inline std::string daycare_sprite_id_to_icon(
    const std::string& daycare_sprite_id,
    const std::array<SpeciesId, 2>& daycare_slot)
{
    int slot_idx = -1;
    if (daycare_sprite_id == "daycare:1") slot_idx = 0;
    else if (daycare_sprite_id == "daycare:2") slot_idx = 1;
    else return "";

    SpeciesId species = daycare_slot[slot_idx];
    if (species == 0) return "";  // Empty slot

    std::string_view icon_name = species_to_icon_type_name(species);
    if (icon_name.empty()) return "";

    return "pokemon_icon:" + std::string(icon_name);
}

} // namespace enginemon
