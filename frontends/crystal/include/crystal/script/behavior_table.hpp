#pragma once
// crystal/script/behavior_table.hpp
// Canonical table of all valid Sem_GameSpecificEvent behavior names.
//
// Source authority: pokecrystal/data/events/special_pointers.asm
// Every Crystal Special that maps to Sem_GameSpecificEvent is listed here
// with its stable behavior name and whether it writes wScriptVar.
//
// Usage:
//   - semantic_legalizer.cpp: consumes at Stage 4 lowering time
//   - full_compiler.cpp: builds CompiledGameData::behavior_names at compile time
//   - legality_gate.cpp: validates Sem_GameSpecificEvent::behavior_name at Stage 5
//
// A Special ID NOT in this table AND not handled by a named semantic rule
// will fail lowering (return {} → unlowered → legality gate rejects).

#include <cstdint>
#include <string_view>

namespace crystal {

struct BehaviorEntry {
    uint16_t    special_id;
    const char* behavior_name;
    bool        writes_script_var;
};

// Canonical table of all Sem_GameSpecificEvent behavior entries.
// Any unrecognized Special ID falls through to unlowered → legality failure.
inline constexpr BehaviorEntry BEHAVIOR_TABLE[] = {
    // Link/Trade/Communications
    {  1, "SetBitsForLinkTradeRequest",       false },
    {  2, "WaitForLinkedFriend",              false },
    {  3, "CheckLinkTimeout_Receptionist",    true  },
    {  4, "TryQuickSave",                     true  },
    {  5, "CheckBothSelectedSameRoom",        true  },
    {  6, "FailedLinkToPast",                 false },
    {  7, "CloseLink",                        false },
    {  8, "WaitForOtherPlayerToExit",         true  },
    {  9, "SetBitsForBattleRequest",          false },
    { 10, "SetBitsForTimeCapsuleRequest",     false },
    { 11, "CheckTimeCapsuleCompatibility",    false },
    { 12, "EnterTimeCapsule",                 false },
    { 13, "TradeCenter",                      false },
    { 14, "Colosseum",                        false },
    { 15, "TimeCapsule",                      false },
    { 16, "CableClubCheckWhichChris",         false },
    { 17, "CheckMysteryGift",                 true  },
    { 18, "GetMysteryGiftItem",               true  },
    { 19, "UnlockMysteryGift",                false },
    // Bug Contest
    { 20, "BugContestJudging",                true  },
    { 21, "CheckPartyFullAfterContest",       true  },
    { 22, "ContestDropOffMons",               true  },
    { 23, "ContestReturnMons",                false },
    { 24, "GiveParkBalls",                    true  },
    { 25, "CheckMagikarpLength",              true  },
    { 26, "MagikarpHouseSign",                false },
    // PC / Services
    { 28, "PokemonCenterPC",                  false },
    { 29, "PlayersHousePC",                   false },
    // Day Care
    { 30, "DayCareMan",                       false },
    { 31, "DayCareLady",                      false },
    { 32, "DayCareManOutside",                false },
    { 33, "MoveDeletion",                     true  },
    { 34, "BankOfMom",                        true  },
    // Transport/Map
    { 35, "MagnetTrain",                      false },
    // Name/Story events
    { 36, "NameRival",                        false },
    { 37, "SetDayOfWeek",                     false },
    { 38, "OverworldTownMap",                 false },
    { 39, "UnownPrinter",                     true  },
    // Game Corner
    { 41, "UnownPuzzle",                      true  },
    { 42, "SlotMachine",                      false },
    { 43, "CardFlip",                         false },
    // Battle Tower / Fade
    { 47, "BattleTowerFade",                  false },
    // Sprites
    { 55, "UpdateSprites",                    false },
    // Pokemon Center heal animation
    { 62, "HealMachineAnim",                  false },
    // Day Care
    { 69, "DayCareMon1",                      true  },
    { 70, "DayCareMon2",                      true  },
    { 71, "SelectRandomBugContestContestants",false },
    // Decorations / Map
    { 73, "ToggleMaptileDecorations",         false },
    { 74, "ToggleDecorationsVisibility",      false },
    // Shuckle events
    { 75, "GiveShuckle",                      false },
    { 76, "ReturnShuckie",                    false },
    { 77, "BillsGrandfather",                 false },
    // Lucky Number / Apricorn
    { 82, "CheckForLuckyNumberWinners",       true  },
    { 83, "CheckLuckyNumberShowFlag",         true  },
    { 84, "ResetLuckyNumberShowFlag",         false },
    { 85, "PrintTodaysLuckyNumber",           false },
    { 86, "SelectApricornForKurt",            true  },
    { 87, "NameRater",                        false },
    // Link record
    { 88, "DisplayLinkRecord",                false },
    // Party happiness/checks
    { 89, "GetFirstPokemonHappiness",         true  },
    { 90, "CheckFirstMonIsEgg",               true  },
    { 93, "RandomPhoneMon",                   false },
    // Snorlax / Grooming
    { 96, "SnorlaxAwake",                     false },
    { 97, "OlderHaircutBrother",              true  },
    { 98, "YoungerHaircutBrother",            true  },
    { 99, "DaisysGrooming",                   true  },
    // Cries / PC
    {100, "PlayCurMonCry",                    false },
    {101, "ProfOaksPCBoot",                   false },
    {103, "TrainerHouse",                     true  },
    {104, "PhotoStudio",                      false },
    {105, "InitRoamMons",                     false },
    // Diploma
    {107, "Diploma",                          false },
    {108, "PrintDiploma",                     false },
    // Battle Tower
    {116, "BattleTowerRoomMenu",              true  },
    {119, "BattleTowerBattle",                true  },
    {122, "LoadOpponentTrainerAndPokemon",    false },
    {124, "CheckForBattleTowerRules",         true  },
    {125, "GiveOddEgg",                       true  },
    {126, "Reset",                            false },
    // Mobile / Function stubs
    {127, "Function1011f1",                   false },
    {128, "Function101220",                   false },
    {129, "Function101225",                   false },
    {130, "Function101231",                   false },
    // Move Tutor / Chambers
    {131, "MoveTutor",                        true  },
    {132, "OmanyteChamber",                   false },
    // Battle Tower action
    {134, "BattleTowerAction",                true  },
    // Unown display
    {135, "DisplayUnownWords",                false },
    // Challenge explanation
    {136, "Menu_ChallengeExplanationCancel",  true  },
    // Mobile errors
    {139, "BattleTowerMobileError",           true  },
    {140, "AskMobileOrCable",                 true  },
    // Chambers
    {141, "HoOhChamber",                      false },
    {143, "CelebiShrineEvent",                false },
    {144, "CheckCaughtCelebi",                true  },
    // PokeSeer / Buena's
    {145, "PokeSeer",                         false },
    {146, "BuenasPassword",                   true  },
    {147, "BuenaPrize",                       true  },
    {148, "GiveDratini",                      false },
    // Beasts / Party checks
    {150, "BeastsCheck",                      true  },
    {151, "MonCheck",                         true  },
    // Mobile
    {154, "Mobile_SelectThreeMons",           true  },
    {155, "Function1037eb",                   true  },
    {156, "Function10383c",                   true  },
    {159, "Function1037c2",                   false },
    {161, "Function103780",                   false },
    {162, "Function10387b",                   false },
};

inline constexpr std::size_t BEHAVIOR_TABLE_SIZE =
    sizeof(BEHAVIOR_TABLE) / sizeof(BEHAVIOR_TABLE[0]);

} // namespace crystal
