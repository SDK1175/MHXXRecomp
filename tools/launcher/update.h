#pragma once
// The launcher's update (update.cpp; MHGU Recomp's separate updater program, merged in): at the start it asks GitHub
// for the latest release of this port, and when it is newer than this one, the launcher shows its patch notes and
// asks; then it downloads the release (checked: its size and SHA-256), installs it over the game folder (never the
// player's files) and starts again. [Application] Updates: Ask (the default), Auto (no question), Off.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

enum UpdateState {
    UPD_IDLE,         // not checked (Off, a development build)
    UPD_CHECKING,
    UPD_NONE,         // up to date, or no answer
    UPD_AVAILABLE,    // a newer release: latest, title, notes
    UPD_DOWNLOADING,  // got / total bytes
    UPD_INSTALLING,
    UPD_DONE,         // installed: the launcher starts again
    UPD_FAILED,       // error says why (nothing installed, or all put back)
};
struct Update {
    std::atomic<int> state{UPD_IDLE};
    std::atomic<int64_t> got{0}, total{0};
    std::atomic<bool> cancel{false};
    std::mutex lock;  // (the strings)
    std::string version;  // this one (version.txt), "" for a development build
    std::string latest, title, notes, error;
};
// At the launcher's start: the last update's leftovers removed; this version read; the check started (a thread) unless
// [Application] Updates is Off or this is a development build. (MHXX_UPDATE_API: another "latest release" address,
// MHXX_UPDATE_FORCE: a development build checks too - tests)
void update_begin(Update *u, int setting /* UPDATES_* */);
// Yes: download and install (a thread)
void update_install(Update *u);
// After UPD_DONE: the new launcher started (true), this one should quit
bool update_restart();
// The game's executable here (MHXX Recomp.exe; mhgurecomp elsewhere)
const char *update_game_exe();
