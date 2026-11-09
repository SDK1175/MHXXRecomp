#pragma once
// The launcher's Tools (tools.cpp): the save's backups, a problem report, the PC's check, the shader caches. (A mod's
// .zip: nxinstall.h install_mod_zip.) Everything here works on paths in UTF-8 and reports errors in English; the
// launcher's Tools screen shows them.
#include <cstdint>
#include <string>
#include <vector>

// ---- launcher.ini (beside the programs): "Key = value" lines
std::string launcher_ini_get(const char *key, const char *fallback = "");
void launcher_ini_set(const char *key, const char *value);

// ---- the save
// The folder the game keeps its save in ([Application] SaveLocation, else %APPDATA%\MHXXRecomp\save, as the game
// does: runtime/main.c, ui.c). "" when it can't be told.
std::string save_dir();
bool save_exists();  // (a save_N folder with something in it)
struct Backup {
    std::string file;  // backups/<this>
    std::string when;  // "2026-11-09 18:30"
    std::string kind;  // manual, start (before the game), update (before an update), restore (before a restore)
    int64_t size;
    int seq = 1;       // (another in the same second: 2, 3 ...)
};
std::vector<Backup> backup_list();  // the newest first
// A .zip of the save folder in backups/ (save-<date>-<kind>.zip). Automatic ones (all but manual) beyond the newest
// 10 are removed. false: err says why (nothing to back up is not an error: made stays "").
bool backup_make(const char *kind, std::string &made, std::string &err);
// The save folder replaced by the backup's (the save as it was first kept as a "restore" backup). Not while the game
// runs (the launcher checks).
bool backup_restore(const std::string &file, std::string &err);
bool backup_delete(const std::string &file);
bool auto_backup_on();  // launcher.ini AutoBackup (on unless 0)

// ---- the PC
struct Spec {
    std::string cpu;          // its name
    int threads = 0;
    bool avx2 = false;
    int ram_mb = 0;
    uint64_t disk_free = 0;   // on the game's drive
    // Vulkan (checked on a thread of its own: vulkan_done)
    bool vulkan_done = false, vulkan = false;
    std::string vulkan_gpu;   // the best device's name
    int vk_major = 0, vk_minor = 0;
    std::string os;
};
void spec_check(Spec &s);      // all but Vulkan, at once
void spec_vulkan(Spec &s);     // Vulkan (slow-ish: a thread)
// What will stop the game (hard) or may trouble it (soft), in English for tr(); none: empty
std::vector<std::string> spec_problems(const Spec &s, bool hard);

// ---- a problem report: reports/report-<date>.zip with the logs, the settings (the multiplayer address and key
// left out), system.txt (the PC, the game files, mods and texture pack); the user's own folder names hidden.
bool report_make(const Spec &spec, const std::string &version, std::string &made, std::string &err);
void open_folder(const char *dir);  // (in the file manager)

// ---- the caches (cache/: shaders and pipelines; the game makes them again)
uint64_t cache_size();
bool cache_clear(uint64_t &freed, std::string &err);

std::string size_text(uint64_t bytes);  // "12.3 MB"
