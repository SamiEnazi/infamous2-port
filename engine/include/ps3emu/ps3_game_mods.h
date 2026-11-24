/*
 * ps3_game_mods -- the game mods the file layer serves (runtime/ppu/ppu_fs_mods.h),
 * for the in-game menu's Mods page (libs/video/vk_overlay.cpp).
 *
 * The list is read once at start and again on ps3_game_mods_rescan() (and after
 * an install from a zip); what a mod changes is decided at start, so a choice
 * made in the menu (on/off, its options, "Use this one instead") is saved at
 * once and takes effect at the next start. The page shows the next start's
 * result at once: the clash rule over the saved choices, without building
 * anything (ps3_game_mod.next / status).
 */
#ifndef PS3EMU_PS3_GAME_MODS_H
#define PS3EMU_PS3_GAME_MODS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char folder[128];     /* the mod's folder, below the mods folder ("Red Jacket", "pack/Red Jacket") */
    char name[96];        /* mod.json "name" (else the folder) */
    char author[64];
    char version[32];
    int  on;              /* the saved choice: used at the next start */
    int  active;          /* in use in this session */
    int  next;            /* used from the next start (the saved choices; no builds) */
    int  for_game;        /* mod.json names inFAMOUS 2 */
    int  n_files;         /* whole files it replaces */
    int  n_tex;           /* texture patches */
    int  n_xpps;          /* packed-file patches */
    int  n_modules;       /* options (modular.json) */
    int  priority;        /* put first by the player (mods.ini priority; 0 = folder order) */
    int  has_image;       /* mod.json names a preview picture (ps3_game_mods_image checks it) */
    int  clash;           /* left out from the next start by a clash, and "Use this one instead" would use it */
    char prefer[240];     /* with clash: what that does ("Climbing Gloves and Karmic Logo Tshirt will be left out") */
    char status[300];     /* one line for the menu: what it changes, or why it is not used */
    int  live;            /* mod.json "live": true -- on and off at once, no restart (patched in memory) */
    int  conflicts;       /* parts it and other mods that are on both change, not resolved (ps3_game_mods_conflicts) */
    char conflicts_with[200]; /* ... with whom ("Climbing Gloves and Karmic Logo Tshirt") */
    char merged_with[200];    /* the mods it is merged with: every part both change merges or was resolved */
} ps3_game_mod;

/* A part of a game file that a mod and another mod that is on both change. */
typedef struct {
    int  other;           /* the other mod's index */
    char other_name[96];
    char file[128];       /* the game file ("cole.xpps") */
    char part[24];        /* "p80" packed file 80, "t1a000" the texture at that data offset, "*" the whole file */
    char what[200];       /* "cole.xpps, packed file 80" */
    int  merges;          /* merged byte by byte (they change different bytes of it): nothing to choose */
    int  winner;          /* the player's choice: 0 this mod's version, 1 the other's, -1 none yet */
    int  live;            /* one of the two is a live mod: the choice applies at once for it */
} ps3_game_mod_conflict;

typedef struct {
    char id[64];
    char name[96];
    char group[96];       /* "" = a toggle of its own */
    char group_id[64];
    int  dropdown;        /* one of a group's options (else a toggle) */
    int  picked;          /* chosen (saved) */
    int  single;          /* a "toggle" group made one-at-a-time: all its options change the same
                             file (the Karmic T-shirt's designs of one texture); dropdown is set too */
} ps3_game_mod_option;

/* a zip file the "Add a mod from a zip" list shows */
typedef struct {
    char path[520];       /* UTF-8 */
    char file[160];
    char where[96];       /* "Downloads", "the mods folder", "chosen", a folder's name */
    long long size, mtime;
    int  ok;              /* it can be installed */
    int  exists;          /* its folder is in the mods folder already: Replace */
    char folder[128];     /* the folder it installs as */
    char what[300];       /* "Sasha's Black Tar 2.0 by sweetasnz -> mods/Sasha's Black Tar", or why it cannot be installed */
} ps3_game_mod_zip;

const char* ps3_game_mods_dir(void);          /* the mods folder ("" = none) */
int  ps3_game_mods_count(void);
int  ps3_game_mods_get(int i, ps3_game_mod* out);
int  ps3_game_mods_option(int i, int k, ps3_game_mod_option* out);
void ps3_game_mods_set_on(int i, int on);    /* saved; next start */
void ps3_game_mods_pick(int i, int k, int on); /* an option on/off (a dropdown's: chosen) */
int  ps3_game_mods_prefer(int i);            /* "Use this one instead": saved; 1 = done */
int  ps3_game_mods_restart_needed(void);     /* the saved choices differ from what this session uses */
/* Conflicts (the Mods page asks when a mod is turned on): the parts mod i and the
 * other mods that are on both change, at most cap of them; returns how many there
 * are. ps3_game_mods_resolve saves whose version each part uses (winners[k] for the
 * list's k-th part, in the same order: 0 mod i's, 1 the other mod's, -1 unchanged);
 * the rest of both mods is used, merged. Returns 0 nothing saved, 1 saved and in
 * use now (live mods only), 2 saved: the game must restart to use it. */
int  ps3_game_mods_conflicts(int i, ps3_game_mod_conflict* out, int cap);
int  ps3_game_mods_resolve(int i, const int* winners, int n);
int  ps3_game_mods_image(int i, char* out, size_t cap); /* the preview picture's path; 1 = one to show */
unsigned ps3_game_mods_generation(void);     /* changes whenever the list or a choice changes */
void ps3_game_mods_rescan(void);             /* re-read the folder for the menu */
int  ps3_game_mods_in_use(void);             /* files served from mods this session */
void ps3_game_mods_open_folder(void);        /* the mods folder in Explorer (Windows) */

/* Add a mod from a zip: one job at a time on a worker thread. */
void ps3_game_mods_zips_refresh(void);       /* list (and check) the zips in Downloads, the mods folder, ... */
int  ps3_game_mods_zip_count(void);
int  ps3_game_mods_zip_get(int i, ps3_game_mod_zip* out);
int  ps3_game_mods_zip_not_mods(void);       /* zips listed that are no mod at all */
int  ps3_game_mods_zip_install(const char* path_utf8, int replace);   /* 0 = busy */
int  ps3_game_mods_zip_choose(void);         /* the Windows file dialog; the pick goes to the top of the list */
int  ps3_game_mods_zip_busy(char* what, size_t cap, float* progress);  /* 1 = a job runs */
int  ps3_game_mods_zip_result(char* out, size_t cap);  /* the last install's report: 1 installed, -1 not, 0 none */
unsigned ps3_game_mods_zip_seq(void);        /* changes when the list or the report does */

#ifdef __cplusplus
}
#endif

#endif
