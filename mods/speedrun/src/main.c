// Speedrun, the example mod. Playing a sub-chapter from the title's CHAPTERS page times the rest of
// that chapter, in a pane on the left, split by sub-chapter in whatever order they are played: the
// clock never stops between them (the pause menu stops it), and cutscenes are skipped while it runs.
// Each split shows how far ahead (-) or behind (+) the best time for that sub-chapter it is; the best
// times are kept in the mod's own data file.
#include "modding.h"
#include "conker.h"

#define SCENE_TITLE 0x1D
#define SCENE_GREGG 0x22  // losing the last life, from anywhere

// osGetCount counts at half the CPU's clock, 46,875,000 a second, and goes round every 91 seconds:
// the clock adds what it moved since the frame before.
#define COUNTS_PER_SECOND 46875000u
#define COUNTS_PER_HUNDREDTH (COUNTS_PER_SECOND / 100)
// A few menus set the text flags to 0x81 again later in the frame; a frame is at least 16 ms.
#define SAME_FRAME (COUNTS_PER_SECOND / 250)

u32 osGetCount(void);

// --- the chapters --------------------------------------------------------------------------------

// The CHAPTERS page's sub-chapters in its order, each with its chapter, the event the page lists it
// by, and the event that ends it in the event flags. A sub-chapter splits when its end is set, in
// whatever order they are played, and its time is from the split before. The page's event is the end
// where nothing better is known; some are set as a sub-chapter starts (POO CABIN's as Conker goes in)
// and some as it is done (MARVIN's as Marvin dies). The game's event table (D_80087430) names
// some events and lists what each needs first.
#define NONE 0xFF
// An end that also ends the run: the game goes on to another chapter's sub-chapters from there.
#define ENDS_RUN 0x100
// An end that splits once Conker leaves the level the event was set in (GARGOYLE's is set as he is
// spoken to).
#define ON_LEAVING 0x200
// An end that also needs a second event (BARRY + CO is done once Frankie is free and 64 is set too).
#define AND(event) ((u32)(event) << 16)
typedef struct {
    u8 chapter;
    u8 page;
    u32 end;
    const char* name;
} SubChapter;

// Most ends come from replaying every sub-chapter from the CHAPTERS page and comparing the events
// each replay sets beforehand with the next one's. SCAREDY BIRDY ends as Conker sobers up at the
// second context pad, PAN HANDLED once the key has opened the door, GREAT BALLS OF POO once the
// mountain door opens, and SUNNY DAYS on the game's "Sunflower Babe" as the bounce starts (its page
// event is "Meet King Bee"); MARVIN's page event is set as Marvin dies.
// MRS BEE ends on its page event, "The Queen's lost her Home!", done as her last cutscene ends (22,
// which it used to wait for, is on the beetles' side of WINDY: it needs 21 and 19).
// WINDY's stretches end the run at MRS BEE (BARN BOYS next), GREAT BALLS OF POO (BATS TOWER) and
// MR BARREL (SPOOKY).
static const SubChapter sub_chapters[] = {
    {0, 0, 1, "SCAREDY BIRDY"}, {0, 4, 5, "PAN HANDLED"}, {0, 6, 6 | ON_LEAVING, "GARGOYLE"},
    {1, 10, 10 | ENDS_RUN, "MRS BEE"}, {1, 117, 118, "POO CABIN"}, {1, 132, 132, "PRUNED"},
    {1, 133, 133, "YEE HAA!"}, {1, 120, 120, "SEWAGE SUCKS"}, {1, 28, 29 | ENDS_RUN, "GREAT BALLS OF POO"},
    {1, 11, 44, "WASPS' REVENGE"}, {1, 39, 39 | ENDS_RUN, "MR BARREL"},
    {2, 69, 69, "MRS CATFISH"}, {2, 85, 85, "BARRY'S MATE"}, {2, 81, 83, "COGS' REVENGE"},
    {2, 70, 71, "THE COMBINATION"}, {2, 75, 76, "BLAST DOORS"}, {2, 77, 80, "CLANG'S LAIR"},
    {2, 134, 134, "PISSTASTIC"}, {2, 135, 135, "BRASS MONKEYS"}, {2, 73, 73, "BULLFISH'S REVENGE"},
    {3, 45, 45, "MARVIN"}, {3, 60, 60, "MAD PITCHFORK"}, {3, 54, 55, "SUNNY DAYS"},
    {3, 61, 63 | AND(64), "BARRY + CO"}, {3, 65, 65, "BUFF YOU"}, {3, 67, 67, "HAYBOT WARS"},
    {3, 68, 68, "FRYING TONIGHT"}, {3, 59, 59, "SLAM DUNK"},
    {4, 122, 122, "CORN OFF THE COB"}, {4, 123, 123, "SWEET MELODY"}, {4, 124, 124, "U-BEND BLUES"},
    {4, 126, 126, "THE BLUFF"},
    {5, 87, 88, "DRUNKEN GITS"}, {5, 102, 89, "SACRIFICE"}, {5, 94, 94, "PHLEGM"},
    {5, 96, 91, "WORSHIP"}, {5, 107, 107, "ROCK SOLID"}, {5, 98, 105, "BOMB RUN"},
    {5, 130, 131, "MUGGED"}, {5, 113, 113, "RAPTOR FOOD"}, {5, 114, 114, "BUGA THE KNUT"},
    {6, 166, 167, "MR DEATH"}, {6, 169, 170, "COUNT BATULA"}, {6, 171, 172, "ZOMBIES"},
    {6, 173, 173, "MR BARREL"},
    {7, 197, 195, "IT'S WAR"}, {7, 158, 158, "POWER'S OFF"}, {7, 159, 159, "TNT"},
    {7, 138, 138, "THE ASSAULT"}, {7, 139, 144, "SOLE SURVIVOR"}, {7, 164, 164, "CASUALTY DEPT."},
    {7, 147, 148, "SAVING PRIVATE RODENT"}, {7, 154, 156, "CHEMICAL WARFARE"}, {7, 155, 155, "THE TOWER"},
    {7, 161, 161, "LITTLE GIRL"}, {7, 162, 162, "THE EXPERIMENT"}, {7, 163, 163, "COUNTDOWN"},
    {7, 141, 160, "PEACE AT LAST!"},
    {8, 42, 42, "THE WINDMILL'S DEAD"}, {8, 177, 177, "ENTER THE VERTEX"}, {8, 182, 182, "THE VAULT"},
    {8, 199, 199, "END CUTSCENE"},
};
#define SUB_CHAPTER_COUNT (s32)(sizeof(sub_chapters) / sizeof(sub_chapters[0]))

static const char* const chapter_names[] = {
    "HUNGOVER", "WINDY", "BATS TOWER", "BARN BOYS", "SLOPRANO", "UGA BUGA", "SPOOKY", "IT'S WAR", "HEIST",
};
#define CHAPTER_COUNT (s32)(sizeof(chapter_names) / sizeof(chapter_names[0]))


static int event_set(s32 event) {
    return event != NONE && event_flags != 0 && (event_flags[event >> 3] & (1 << (event & 7)));
}

// --- the run -------------------------------------------------------------------------------------

enum RunState { RUN_OFF, RUN_WAITING, RUN_RUNNING, RUN_FINISHED };
// Each of the chapter's sub-chapters in the run.
enum Row { ROW_TO_COME, ROW_DONE, ROW_SKIPPED };

static enum RunState state;
static s32 chapter;
static s32 first;  // the sub-chapter the replay started at (an index in sub_chapters)
static s32 chapter_first;  // and the chapter's first and last
static s32 last;
static u8 row[SUB_CHAPTER_COUNT];
static u8 opened[SUB_CHAPTER_COUNT];     // its split was the run's first
static u8 was_set[SUB_CHAPTER_COUNT];    // its page event was set when the run began (every chapter's)
static s8 left_from[SUB_CHAPTER_COUNT];  // for an ON_LEAVING end: the scene it was set in, or -1
static u32 split_at[SUB_CHAPTER_COUNT];  // the run's time when each ended
static u32 started_at[SUB_CHAPTER_COUNT];
static u32 last_split;  // the run's time at its last split
static u32 last_count;
static u32 counts;  // toward the next hundredth
static u32 hundredths;
static int holding_l;

// The best times, kept in the mod's data file: each sub-chapter's own, after another split of the run
// (after) or as the run's first (opened, from the replay's start, which leaves out what came before
// it in the chapter), and each chapter's from each sub-chapter a replay can start at; 0 for none yet.
#define BEST_MAGIC 0x43534232  // "CSB2"
typedef struct {
    u32 magic;
    u32 after[64];
    u32 opened[64];
    u32 from[64];
} Best;
static Best best;
static int best_loaded;
// The best times as they were when the run started, which its splits are compared with.
static Best previous;

static void load_best(void) {
    if (best_loaded) {
        return;
    }
    best_loaded = 1;
    if (conker_mod_data_load(&best, sizeof(best)) != sizeof(best) || best.magic != BEST_MAGIC) {
        u32 i;
        for (i = 0; i < 64; i++) {
            best.after[i] = 0;
            best.opened[i] = 0;
            best.from[i] = 0;
        }
        best.magic = BEST_MAGIC;
    }
}

// The best a sub-chapter's split is compared with.
static u32 best_split(const Best* times, s32 i) {
    return opened[i] || (row[i] == ROW_TO_COME && i == first) ? times->opened[i] : times->after[i];
}

// The CHAPTERS page plays the chosen sub-chapter: the run waits for its first frame past the title.
RECOMP_HOOK("func_151E30C4") void splits_start(void) {
    s32 i;
    s32 nth = 0;

    state = RUN_OFF;
    if (chapters_chapter < 0 || chapters_chapter >= CHAPTER_COUNT) {
        return;
    }
    chapter = chapters_chapter;
    first = -1;
    chapter_first = -1;
    for (i = 0; i < SUB_CHAPTER_COUNT; i++) {
        if (sub_chapters[i].chapter != chapter) {
            continue;
        }
        if (chapter_first < 0) {
            chapter_first = i;
        }
        if (nth++ == chapters_sub_chapter) {
            first = i;
        }
        last = i;
    }
    if (first < 0) {
        return;
    }
    load_best();
    for (i = 0; i < 64; i++) {  // (no memcpy for a struct copy)
        previous.after[i] = best.after[i];
        previous.opened[i] = best.opened[i];
        previous.from[i] = best.from[i];
    }
    counts = 0;
    hundredths = 0;
    state = RUN_WAITING;
}

// An end is reached once its event is done: its status says so at once, while its flag can wait
// for the next load (MRS BEE's does).
static int done(s32 event) {
    return event_set(event) || (event != NONE && event_status[event] == EVENT_DONE);
}

static int ended(s32 i) {
    u32 end = sub_chapters[i].end;
    return done(end & 0xFF) && (end >> 16 == 0 || done((end >> 16) & 0xFF));
}

// The run's first frame in play: the sub-chapters before the chosen one, and any other the replay has
// already marked done, are left out.
static void begin(void) {
    s32 i;
    for (i = 0; i < SUB_CHAPTER_COUNT; i++) {
        was_set[i] = event_set(sub_chapters[i].page);
        left_from[i] = -1;
    }
    for (i = chapter_first; i <= last; i++) {
        // The replay has done the ones before it (some leave no event set, like SUNNY DAYS).
        row[i] = i < first || (i != first && ended(i)) ? ROW_SKIPPED : ROW_TO_COME;
        opened[i] = 0;
    }
    last_split = 0;
}

// Sub-chapter `i` has ended (-1 for none, when another chapter has begun): its split, and new bests
// are kept. Once none is left to come, or `ending`, the run is finished.
static void split(s32 i, int ending) {
    s32 j;
    u32* kept;
    int better = 0;

    if (i >= 0) {
        opened[i] = last_split == 0;
        kept = opened[i] ? &best.opened[i] : &best.after[i];
        started_at[i] = last_split;
        split_at[i] = hundredths;
        row[i] = ROW_DONE;
        last_split = hundredths;
        if (*kept == 0 || split_at[i] - started_at[i] < *kept) {
            *kept = split_at[i] - started_at[i];
            better = 1;
        }
    }
    state = RUN_FINISHED;
    for (j = chapter_first; j <= last && !ending; j++) {
        if (row[j] == ROW_TO_COME) {
            state = RUN_RUNNING;
        }
    }
    if (state == RUN_FINISHED && (best.from[first] == 0 || hundredths < best.from[first])) {
        best.from[first] = hundredths;
        better = 1;
    }
    if (better) {
        conker_mod_data_save(&best, sizeof(best));
    }
}

// Once a frame while the clock runs. Chapters' sub-chapters are played in between each other's
// (WINDY's with BARN BOYS'), so another chapter's page event being set ends the run: a run times one
// unbroken stretch of its chapter.
static void follow(void) {
    s32 i;

    // The chapter's own ends first: a cutscene can set one with the next chapter's page event.
    for (i = chapter_first; i <= last && state == RUN_RUNNING; i++) {
        u32 end = sub_chapters[i].end;
        if (row[i] != ROW_TO_COME || !ended(i)) {
            continue;
        }
        if (!(end & ON_LEAVING)) {
            split(i, (end & ENDS_RUN) != 0);
        } else if (left_from[i] < 0) {
            left_from[i] = current_scene;
        } else if (current_scene != left_from[i] && current_scene != SCENE_GREGG) {
            split(i, (end & ENDS_RUN) != 0);
        }
    }
    for (i = 0; i < SUB_CHAPTER_COUNT && state == RUN_RUNNING; i++) {
        if (sub_chapters[i].chapter != chapter && !was_set[i] && event_set(sub_chapters[i].page)) {
            split(-1, 1);
        }
    }
}

// --- the pane ------------------------------------------------------------------------------------

#define PANE_X 8  // from the window's left edge (HUD_TEXT_LEFT_EDGE)
#define PANE_Y 30
#define ROW_HEIGHT 8
// The game queues all text into one display list, with a texture load for every glyph, and never
// checks it for room: IT'S WAR's 13 sub-chapters overran it (a heap overflow in scene 19). So the
// pane shows a window of the sub-chapters, up to 3 done before the one being played.
#define PANE_ROWS 7
#define ROWS_BEFORE 3
#define PANE_SCALE 0.6f
#define TIME_X 104    // where a time's digits start (past the longest name)
#define DELTA_X 144   // and the difference from the best
// The time's characters each have a place of their own: the game's letters are not all as wide, so a
// time drawn as one text would shift about as its digits change.
#define DIGIT_WIDTH 5
#define MARK_WIDTH 2  // ':'
// The hundredths are smaller, on the same baseline, after a '.' with room to show (the font's '.' is
// a small raised dot, which the next digit's shadow covered).
#define POINT_WIDTH 3
#define SMALL_SCALE 0.45f
#define SMALL_DIGIT_WIDTH 4
#define SMALL_DROP 1

#define COLOR_TITLE 0xFFE070FF
#define COLOR_DONE 0xFFFFFFFF
#define COLOR_PLAYING 0x80E0FFFF
#define COLOR_TO_COME 0x909090FF
#define COLOR_AHEAD 0x60FF60FF
#define COLOR_BEHIND 0xFF6060FF

static void text(s32 x, s32 y, u32 color, const char* format, s32 a0, s32 a1) {
    hud_text_color = color;
    func_15042D94(x, y, HUD_TEXT_SHADOW | HUD_TEXT_LEFT_EDGE, format, a0, a1);
}

static s32 digit(s32 x, s32 y, u32 color, u32 value) {
    text(x, y, color, "%d", value % 10, 0);
    return x + DIGIT_WIDTH;
}

static s32 mark(s32 x, s32 y, u32 color, const char* mark) {
    text(x, y, color, mark, 0, 0);
    return x + MARK_WIDTH;
}

// MM:SS.CC (minutes past 99 go round).
static void draw_time(s32 y, u32 color, u32 time) {
    u32 seconds = time / 100;
    s32 x = TIME_X;
    x = digit(x, y, color, seconds / 600);
    x = digit(x, y, color, seconds / 60);
    x = mark(x, y, color, ":");
    x = digit(x, y, color, seconds / 10 % 6);
    x = digit(x, y, color, seconds);
    text(x, y, color, ".", 0, 0);
    x += POINT_WIDTH;
    func_150432BC(SMALL_SCALE);
    text(x, y + SMALL_DROP, color, "%d", time / 10 % 10, 0);
    text(x + SMALL_DIGIT_WIDTH, y + SMALL_DROP, color, "%d", time % 10, 0);
    func_150432BC(PANE_SCALE);
}

// +S.CC behind the best, -S.CC ahead of it.
static void draw_delta(s32 y, u32 time, u32 best_time) {
    u32 apart;
    if (best_time == 0) {
        return;
    }
    if (time > best_time) {
        apart = time - best_time;
        text(DELTA_X, y, COLOR_BEHIND, "+%d.%02d", apart / 100, apart % 100);
    } else {
        apart = best_time - time;
        text(DELTA_X, y, COLOR_AHEAD, "-%d.%02d", apart / 100, apart % 100);
    }
}

static void draw_pane(void) {
    s32 i;
    s32 y = PANE_Y;
    // Text queued by func_15042D94 picks up where the game's own left off, so its place, flags,
    // color and scale are put back for the game's next text.
    s16 x = hud_text_x;
    s16 text_y = hud_text_y;
    u8 flags = hud_text_flags;
    u32 color = hud_text_color;
    f32 scale = hud_text_scale;
    u32 total_color;
    s32 playing;
    s32 shown;
    s32 anchor;
    s32 from;

    func_150432BC(PANE_SCALE);
    text(PANE_X, y, COLOR_TITLE, chapter_names[chapter], 0, 0);
    y += ROW_HEIGHT + 2;
    playing = -1;  // the first to come, in the page's order
    for (i = chapter_first; i <= last && state == RUN_RUNNING; i++) {
        if (row[i] == ROW_TO_COME) {
            playing = i;
            break;
        }
    }
    shown = 0;  // the window: from the anchor (the one playing, or the last done) back ROWS_BEFORE
    anchor = 0;
    for (i = chapter_first; i <= last; i++) {
        if (row[i] == ROW_SKIPPED) {
            continue;
        }
        if (i == playing || (playing < 0 && row[i] == ROW_DONE)) {
            anchor = shown;
        }
        shown++;
    }
    from = anchor - ROWS_BEFORE;
    if (from > shown - PANE_ROWS) {
        from = shown - PANE_ROWS;
    }
    if (from < 0) {
        from = 0;
    }
    shown = 0;
    for (i = chapter_first; i <= last; i++) {
        if (row[i] == ROW_SKIPPED) {
            continue;
        }
        shown++;
        if (shown - 1 < from || shown - 1 >= from + PANE_ROWS) {
            continue;
        }
        if (row[i] == ROW_DONE) {
            u32 time = split_at[i] - started_at[i];
            text(PANE_X, y, COLOR_DONE, sub_chapters[i].name, 0, 0);
            draw_time(y, COLOR_DONE, time);
            draw_delta(y, time, best_split(&previous, i));
        } else if (i == playing) {
            text(PANE_X, y, COLOR_PLAYING, sub_chapters[i].name, 0, 0);
            draw_time(y, COLOR_PLAYING, hundredths - last_split);
        } else {
            text(PANE_X, y, COLOR_TO_COME, sub_chapters[i].name, 0, 0);
            if (best_split(&best, i) != 0) {
                draw_time(y, COLOR_TO_COME, best_split(&best, i));
            }
        }
        y += ROW_HEIGHT;
    }
    y += 2;
    // Once finished: green for a new best, red for slower, white for the first from here.
    total_color = COLOR_DONE;
    if (state == RUN_FINISHED && previous.from[first] != 0) {
        total_color = hundredths < previous.from[first] ? COLOR_AHEAD : COLOR_BEHIND;
    }
    text(PANE_X, y, total_color, "TOTAL", 0, 0);
    draw_time(y, total_color, hundredths);
    if (state == RUN_FINISHED) {
        draw_delta(y, hundredths, previous.from[first]);
    }

    hud_text_x = x;
    hud_text_y = text_y;
    hud_text_flags = flags;
    hud_text_color = color;
    hud_text_scale = scale;
}

// --- once a frame --------------------------------------------------------------------------------

// func_151E51EC, the menu handler the game runs once a frame on every screen, sets the text flags
// to 0x81 before it queues any text. That function itself cannot be hooked (the port changes it for
// its debug menu), so the mod hooks func_15042D78 and looks for that call.
RECOMP_HOOK("func_15042D78") void splits_frame(u8 flags) {
    u32 now;
    u32 moved;

    if (flags != 0x81) {
        return;
    }
    now = osGetCount();
    moved = now - last_count;  // right across a wrap too
    if (moved < SAME_FRAME) {
        return;
    }
    last_count = now;
    if (current_scene == SCENE_TITLE) {
        state = RUN_OFF;  // quit, or the next replay is being chosen
    }
    if (state == RUN_WAITING && current_scene != SCENE_TITLE) {
        state = RUN_RUNNING;
        begin();
    }
    if (state == RUN_RUNNING && !pause_open) {
        counts += moved;
        hundredths += counts / COUNTS_PER_HUNDREDTH;
        counts %= COUNTS_PER_HUNDREDTH;
    }
    if (state == RUN_RUNNING) {
        follow();
    }
    if (state == RUN_RUNNING || state == RUN_FINISHED) {
        draw_pane();
    }
}

// While the clock runs, L is held for the game as each playing cutscene runs its frame, which skips
// it as soon as the game allows. Let go again after, so play never sees L.
RECOMP_HOOK("func_1501E400") void splits_skip(s32 slot) {
    if (state == RUN_RUNNING && cutscene_playing[slot] == 1 && !(pad_held & PAD_L)) {
        pad_held |= PAD_L;
        holding_l = 1;
    }
}

RECOMP_HOOK_RETURN("func_1501E400") void splits_skip_done(void) {
    if (holding_l) {
        pad_held &= ~PAD_L;
        holding_l = 0;
    }
}
