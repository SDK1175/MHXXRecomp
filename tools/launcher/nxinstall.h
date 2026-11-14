#pragma once
// The launcher's game file setup (nxinstall.cpp): from the player's own Switch files - the game (NSP or XCI), its
// 1.5.1 update (NSP) and their console's keys (prod.keys) - the exefs and romfs the game runs from, in data/; and the
// Korean patch (.zip) into mods/. Nothing of the game is in the launcher: everything comes from the player's files.
#include <atomic>
#include <mutex>
#include <string>

struct InstallJob {
    // in
    std::string base, update, keys, patch;  // (patch: optional, a .zip)
    std::string game_dir = "data", mods_dir = "mods";
    // out, while it runs (read from the UI thread)
    std::atomic<int> stage{0};        // InstallStage
    std::atomic<uint64_t> done{0}, total{0};
    std::atomic<bool> finished{false}, ok{false}, cancel{false};
    std::mutex lock;
    std::string error;    // (lock: when finished && !ok, why - in English, the UI translates the start)
    std::string current;  // (lock: the file being written)
};
enum InstallStage {
    INSTALL_KEYS = 1,
    INSTALL_BASE,
    INSTALL_UPDATE,
    INSTALL_EXEFS,
    INSTALL_ROMFS,
    INSTALL_PATCH,
    INSTALL_SWAP,
    INSTALL_DONE,
};
// Runs the job (on a thread of its own); a log of what it found goes to install.log next to the launcher.
void install_run(InstallJob *job);
// A mod's .zip into mods/ (the Tools screen): one folder named as the zip (a mod already there by that name is
// replaced). name: that folder's name. false: err says why (in English).
bool install_mod_zip(const std::string &zip, const std::string &mods, std::string &name, std::string &err);
