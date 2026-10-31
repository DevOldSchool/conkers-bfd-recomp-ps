#ifndef MODDING_H
#define MODDING_H

// Markers RecompModTool reads from a mod's ELF: each puts a function in a section whose name says
// what the loader does with it. Hook, callback and import sections carry the target in their name,
// so mods/mod.ld keeps them apart instead of merging them.

// Replaces the game function of the same name (fails to load if another mod replaced it too).
#define RECOMP_PATCH __attribute__((section(".recomp_patch")))
// Replaces it even where the recomp itself already patched it.
#define RECOMP_FORCE_PATCH __attribute__((section(".recomp_force_patch")))
// Runs at the start of the named game function, with the same arguments.
#define RECOMP_HOOK(func) __attribute__((section(".recomp_hook." func)))
// Runs just before the named game function returns.
#define RECOMP_HOOK_RETURN(func) __attribute__((section(".recomp_hook_return." func)))
// Makes a function callable by other mods.
#define RECOMP_EXPORT __attribute__((section(".recomp_export")))
// Runs when `mod` (or "*" for the recomp) fires `event`.
#define RECOMP_CALLBACK(mod, event) __attribute__((section(".recomp_callback." mod ":" #event)))

// Declares an event other mods can attach callbacks to; calling it fires it.
#define RECOMP_DECLARE_EVENT(func) \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"") \
    __attribute__((noinline, weak, used, section(".recomp_event"))) void func {} \
    _Pragma("GCC diagnostic pop")

// Declares a function exported by another mod, or by the recomp itself when `mod` is "*".
#define RECOMP_IMPORT(mod, func) \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"") \
    _Pragma("GCC diagnostic ignored \"-Wreturn-type\"") \
    __attribute__((noinline, weak, used, section(".recomp_import." mod))) func {} \
    _Pragma("GCC diagnostic pop")

#endif
