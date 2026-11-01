#ifndef CONKER_H
#define CONKER_H

// The game's functions and data that mods can use, named as in the decomp (conkers-bfd-decomp).
// Functions link by name against config/us.syms.toml and data against config/us.data.syms.toml
// (the decomp's placeholder names, D_<address>, until it names them). The lower-case names below
// are this header's own, for what is known about each.

typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed int s32;
typedef unsigned int u32;
typedef float f32;

// Memory no symbol names (free RAM, say), by its address.
#define CONKER_DATA(type, address) (*(volatile type*)(address))

// --- HUD text (src/game/game_70200.c) ------------------------------------------------------------

// Queues a line of HUD text at (x, y) in the 292x216 picture. `format` is the game's format string
// ('%' and '#' are format codes); up to 16 argument words follow it.
void func_15042D94(s32 x, s32 y, u8 flags, const char* format, ...);
// Sets the scale of the text queued after it.
void func_150432BC(f32 scale);
// Sets the flags of the text queued after it.
void func_15042D78(u8 flags);
// Queues a format string with its 16 argument words (what func_15042D94 calls).
void func_15042ECC(const char* format, s32* argument_words);

#define HUD_TEXT_SHADOW 0x80    // flags: a black copy one pixel down and right
#define HUD_TEXT_CENTERED 0x01  // flags: centered on x instead of starting there
#define HUD_CENTER_X 0x92       // the middle of the picture

extern u32 D_800CBD60;
extern s16 D_800CBD70;
extern s16 D_800CBD72;
extern u8 D_800CBD74;
extern f32 D_800CBD80;
#define hud_text_color D_800CBD60  // RGBA for the text queued after it
#define hud_text_x D_800CBD70
#define hud_text_y D_800CBD72
#define hud_text_flags D_800CBD74
#define hud_text_scale D_800CBD80  // its scale (what func_150432BC sets)

// --- game state ----------------------------------------------------------------------------------

extern s32 D_800BE9F0;
extern u16 D_800BE930[4];
#define current_scene D_800BE9F0
#define pad_pressed D_800BE930  // buttons pressed this frame, per controller port

// --- cheats --------------------------------------------------------------------------------------

// The cheats entered on the CHEATS page, one bit each. The page (func_151DE8F0) matches what is
// typed against the codes in text bank 0x1C, stored a character position per string with one code
// per column: column n sets bit n.
extern u32 D_800E9D00;
#define cheat_flags D_800E9D00

#define CHEAT_CHOCOLATESTARFISH (1u << 1)
#define CHEAT_BEELZEBUBSBUM (1u << 2)
#define CHEAT_SPUNKJOCKEY (1u << 3)
#define CHEAT_EASTEREGGSRUS (1u << 4)        // multiplayer: Neo Conker
#define CHEAT_WELLYTOP (1u << 5)             // multiplayer: Conker
#define CHEAT_CHINDITVICTORY (1u << 6)       // multiplayer: the Weasel henchmen
#define CHEAT_EATBOX (1u << 7)               // multiplayer: the Caveman
#define CHEAT_BEEFCURTAINS (1u << 8)         // multiplayer: the Zombies and Villagers
#define CHEAT_BOVRILBULLETHOLE (1u << 9)
#define CHEAT_DRACULASTEABAGS (1u << 10)
#define CHEAT_DUTCHOVENS (1u << 11)
#define CHEAT_SPANIELSEARS (1u << 12)
#define CHEAT_MONKEYSCHIN (1u << 14)
#define CHEAT_WELDERSBENCH (1u << 15)
#define CHEAT_RUSTYSHERIFFSBADGE (1u << 16)  // multiplayer: the Sergeant and the Tediz Leader
#define CHEAT_BILLYMILLROUNDABOUT (1u << 17) // multiplayer: Gregg
#define CHEAT_EASY (1u << 22)
#define CHEAT_VERYEASY (1u << 23)
#define CHEAT_ANCHOVYBAY (1u << 24)
#define CHEAT_PRINCEALBERT (1u << 25)
#define CHEAT_CLAMPIRATE (1u << 26)

#endif
