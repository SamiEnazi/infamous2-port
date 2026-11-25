/*
 * ps3_mod_plugin.h -- code that a mod carries (mod.json "plugin"), for the port's
 * mod manager (F2 > Mods). Version 1.
 *
 * A mod can bring a native plugin next to its files:
 *
 *     <mod>/mod.json      {"game": "inf2", "name": "...", "plugin": "plugin.dll"}
 *     <mod>/plugin.dll    exports ps3_mod_plugin_init (below)
 *
 * The manager loads it when the mod is on, "Mods may run code" is on and the player
 * allowed its code (the Mods page asks once per plugin file, naming it with its size
 * and SHA-256: a changed file asks again) -- at start, or at once when that happens
 * while the game runs. It loads only a plugin whose imports are all Windows system
 * DLLs (so the file the player allowed is all the code it brings), from a verified
 * copy of the file, and calls ps3_mod_plugin_init
 * with the host table. The plugin keeps the table (it lives as long as the game)
 * and sets its hooks there. Switching the mod off takes its hooks out at once and
 * on again puts them back (ps3_mod_plugin_enable, below); the DLL stays loaded
 * until the game ends, so a new plugin file takes a restart. A plugin never links
 * against the port: every
 * service is in the table, so one plugin works with any port build whose
 * api >= the version it needs.
 *
 * Hooks: the game's functions that the port's lift gave a hook slot (the port's
 * cheats/hooks.txt) can be hooked by any number of plugins; they are chained:
 *
 *     static const ps3_mod_host* H;
 *     static void my_update(ps3_ctx* ctx, ps3_mod_next next)
 *     {
 *         const uint32_t self = (uint32_t)H->gpr(ctx, 3);   // the arguments, before
 *         next(ctx);                                          // the rest of the chain, then the game's function
 *         ...                                                 // its results, after
 *     }
 *     PS3_MOD_PLUGIN_EXPORT int ps3_mod_plugin_init(const ps3_mod_host* h)
 *     {
 *         if (h->api < 1) return 0;
 *         H = h;
 *         return h->hook_add(h, 0x001578F8, my_update);       // 0: no such slot in this build
 *     }
 *
 * A hook runs on the guest thread that called the function: no blocking, guest
 * memory through the table (guest addresses, big-endian values handled for you).
 * A hook that does not call next replaces the function for that call.
 */
#ifndef PS3EMU_PS3_MOD_PLUGIN_H
#define PS3EMU_PS3_MOD_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3_MOD_PLUGIN_API 1

typedef struct ppu_context ps3_ctx;                         /* the guest thread's registers (opaque: use the table) */
typedef void (*ps3_mod_next)(ps3_ctx* ctx);                 /* the rest of a hook chain / a game function */
typedef void (*ps3_mod_hook)(ps3_ctx* ctx, ps3_mod_next next);

/* The game's save files. file: its name in the save ("STATE"); off: where in the file buf starts.
 * A read callback gets the bytes the game just read, before the game sees them, and may change them; a write
 * callback sees what the game just wrote. Called on the game's thread, only while the mod is on. */
typedef void (*ps3_mod_save_read)(const char* file, unsigned char* buf, size_t size, unsigned long long off);
typedef void (*ps3_mod_save_write)(const char* file, const unsigned char* buf, size_t size, unsigned long long off);

/* A row the plugin shows under its mod in F2 > Mods (the rows callback fills them each time the page
 * is drawn: quick, no blocking). */
typedef struct ps3_mod_row {
    char id[32];             /* the plugin's own name for the row: press(id) gets it */
    char label[80];          /* "Reset First island" */
    char button[24];         /* "Reset" */
    char desc[320];          /* what it does (the menu's help line) */
    char status[200];        /* "" or a line under it ("Waiting for a save to load") */
    int  disabled;           /* 1: shown, its button does nothing */
    int  kind;               /* 0 an action (press(id)); 1 "restart the game": the menu asks the player and goes
                                back to the title screen itself (press is not called; shown only where the port
                                can restart); 2 a line of text, no button */
} ps3_mod_row;
typedef int (*ps3_mod_rows_fn)(ps3_mod_row* out, int cap);   /* how many it filled (at most cap) */
typedef void (*ps3_mod_press_fn)(const char* id);           /* on the menu's thread */

typedef struct ps3_mod_host {
    uint32_t api;            /* PS3_MOD_PLUGIN_API of the port that loaded the plugin */
    uint32_t size;           /* sizeof(ps3_mod_host) there: newer ports only add fields at the end */
    const char* mod_folder;  /* the mod's folder, full path, UTF-8, '/' separators */
    const char* mod_name;    /* mod.json "name" */

    /* the log (stderr.log), one line per call; "[<mod name>] " is put in front. self: the table it got */
    void (*log)(const struct ps3_mod_host* self, const char* fmt, ...);
    /* mod.json: the value of a top-level key as JSON text ("2.5", "true", "{...}"), or NULL */
    const char* (*json)(const struct ps3_mod_host* self, const char* key);
    /* seconds, steady clock */
    double (*now)(void);

    /* guest memory (guest addresses; values in host order) */
    int      (*valid)(uint32_t ea, uint32_t len);           /* 1: mapped guest memory */
    uint8_t  (*read8)(uint32_t ea);
    uint32_t (*read32)(uint32_t ea);
    float    (*readf)(uint32_t ea);
    void     (*write8)(uint32_t ea, uint8_t v);
    void     (*write32)(uint32_t ea, uint32_t v);
    void     (*writef)(uint32_t ea, float v);

    /* registers of a hooked call */
    uint64_t (*gpr)(ps3_ctx* ctx, int i);
    void     (*set_gpr)(ps3_ctx* ctx, int i, uint64_t v);
    double   (*fpr)(ps3_ctx* ctx, int i);
    void     (*set_fpr)(ps3_ctx* ctx, int i, double v);
    void     (*vr)(ps3_ctx* ctx, int i, float out[4]);      /* a vector register as 4 floats */
    void     (*set_vr)(ps3_ctx* ctx, int i, const float v[4]);
    uint64_t (*lr)(ps3_ctx* ctx);                           /* the caller's return address */

    /* hooks (chained; see above) */
    int (*hook_exists)(uint32_t addr);                      /* 1: this build has a hook slot there */
    int (*hook_add)(const struct ps3_mod_host* self, uint32_t addr, ps3_mod_hook fn);   /* 1: added (also before the
                                                               game module is bound: kept for its slot); 0: no room */
    int (*hook_remove)(const struct ps3_mod_host* self, uint32_t addr, ps3_mod_hook fn);

    /* the game's own functions */
    ps3_mod_next (*game_fn)(uint32_t addr);                 /* a lifted function by guest address, NULL: none */
    /* From inside a hook (a call boundary: the guest stack below r1 is free): runs game function
     * addr with ctx's registers as setup(ctx, user) leaves them; result(ctx, user), when given, sees the
     * registers it returned (r3, f1, ...); then ctx is put back as it was. 1: it ran */
    int (*call_game)(ps3_ctx* ctx, uint32_t addr, void (*setup)(ps3_ctx* ctx, void* user),
                     void (*result)(ps3_ctx* ctx, void* user), void* user);

    const char* data_dir;    /* a folder for the plugin's own files, made for it (the mods folder's .port_cache/<mod
                                folder>): kept across games and mod updates; '/' separators, no '/' at the end */
    /* the game's save files (see ps3_mod_save_read); either may be NULL. 1: set */
    int (*on_save)(const struct ps3_mod_host* self, ps3_mod_save_read read, ps3_mod_save_write write);
    /* rows under the mod in F2 > Mods, shown while the mod is on and its code runs. 1: set */
    int (*menu)(const struct ps3_mod_host* self, ps3_mod_rows_fn rows, ps3_mod_press_fn press);
} ps3_mod_host;

/* The plugin's export. Return 1 when it is set up, 0 to say it is not used (the log says so). */
typedef int (*ps3_mod_plugin_init_fn)(const ps3_mod_host* host);
/* Optional export ps3_mod_plugin_enable: the player switched the mod off (0) or on again (1) while the
 * game runs. Before an "off" the plugin's hooks still run; after it returns they are taken out (and put
 * back before an "on"). Undo here what the plugin changed in guest memory. Called on the menu's thread. */
typedef void (*ps3_mod_plugin_enable_fn)(int on);

#ifdef __cplusplus
#define PS3_MOD_PLUGIN_C extern "C"
#else
#define PS3_MOD_PLUGIN_C
#endif
#if defined(_WIN32)
#define PS3_MOD_PLUGIN_EXPORT PS3_MOD_PLUGIN_C __declspec(dllexport)
#else
#define PS3_MOD_PLUGIN_EXPORT PS3_MOD_PLUGIN_C __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}
#endif

#endif
