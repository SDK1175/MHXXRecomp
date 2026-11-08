// mhxx-launcher: the game's launcher. A window of its own (SDL's 2D renderer, no Vulkan) with the game's settings
// menu (runtime/settings_panel.cpp) showing only what takes effect when the game starts (window, resolution,
// render resolution, FSR, anisotropic filtering, the save's folder, mods ...), and Start game / Quit. Start game
// saves the settings and starts mhgurecomp next to it, which then reads them; the game's own Exit to launcher (F1)
// starts this again.
//
// Everything else stays in the game's menu (F1), which keeps these settings too.
//
// Without the game files (or from "Install game files") it first shows the setup: the player picks their own game
// (NSP / XCI), its 1.5.1 update, their console's keys and, if they like, the Korean patch's .zip, and nxinstall.cpp
// puts the game files in data/ and the patch in mods/.
#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "app_actions.h"
#include "config.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "nxinstall.h"
#include "texmake.h"
#include "settings_locale.h"
#include "settings_panel.h"
#include "title_art.h"

// ---------------------------------------------------------------- what the panel asks of the game (none here)
extern "C" {
const char *g_game_dir = "data";  // (modorder.c: the mods' archives against the game's)
static SDL_AtomicInt want_folder;  // ui_request_import(4): the save's folder picker
static char picked[1100];
static SDL_AtomicInt picked_ready;
void ui_request_action(bool restart) { (void)restart; }
void ui_request_exit_to_launcher(void) {}
bool ui_action_failed(void) { return false; }
void ui_request_import(int what) {
    if (what == 4) SDL_SetAtomicInt(&want_folder, 1);
}
int ui_take_picked_folder(char *out, size_t size) {
    if (!SDL_GetAtomicInt(&picked_ready)) return 0;
    SDL_strlcpy(out, picked, size);
    SDL_SetAtomicInt(&picked_ready, 0);
    return 4;
}
int ldn_hub_status(const char *, const char *, int *, int *) { return 0; }
void ldn_hub_check(const char *, const char *) {}
int cheats_count(void) { return 0; }
void cheats_rescan(void) {}
const char *cheats_name(int) { return ""; }
const char *cheats_file(int) { return ""; }
bool cheats_master(int) { return false; }
bool cheats_on(int) { return false; }
bool cheats_set(const bool *, int, char *, size_t) { return true; }
bool cheats_add(const char *, char *err, size_t n) {
    SDL_strlcpy(err, "", n);
    return false;
}
int overlay_face_position(int semantic) {  // (positional: Confirm the bottom button, Back the right one)
    const int p[] = {1, 0, 3, 2};
    return semantic >= 0 && semantic < 4 ? p[semantic] : 0;
}
int host_pad_layout(void) { return -1; }
}

static const char *tr(const char *s) { return settings_text(g_config.language, s); }

static void folder_picked(void *, const char *const *list, int) {
    if (!list || !list[0]) return;
    SDL_strlcpy(picked, list[0], sizeof picked);
    SDL_SetAtomicInt(&picked_ready, 1);
}

// The menu's font with Hangul merged (as the game's overlay: fonts/menu.ttf, else the system's)
static const char *korean_font() {
    static const char *const paths[] = {"fonts/menu.ttf", "fonts/menu.otf", "fonts/menu.ttc",
#ifndef _WIN32
                                        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                                        "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
#endif
                                        nullptr};
    static char win[512];
    for (const char *const *p = paths; *p; p++)
        if (SDL_GetPathInfo(*p, nullptr)) return *p;
#ifdef _WIN32
    const char *w = SDL_getenv("WINDIR");
    SDL_snprintf(win, sizeof win, "%s\\Fonts\\malgun.ttf", w && *w ? w : "C:\\Windows");
    if (SDL_GetPathInfo(win, nullptr)) return win;
#endif
    return nullptr;
}

// ---------------------------------------------------------------- the desktop shortcut (asked at the first start)
// launcher.ini beside the programs remembers that it was asked (Shortcut = asked)
#define LAUNCHER_INI "launcher.ini"
static bool shortcut_asked() {
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(LAUNCHER_INI, &n);
    bool asked = d && strstr(d, "ShortcutAsked = 1");
    SDL_free(d);
    return asked;
}
static void shortcut_remember() {
    const char *text = "; mhxx-launcher\nShortcutAsked = 1\n";
    SDL_SaveFile(LAUNCHER_INI, text, strlen(text));
}

#ifdef _WIN32
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
// A shortcut to this launcher named name.lnk on the user's desktop (its icon: the game's). false: error says why.
static bool make_shortcut(const char *name_utf8, std::string &error) {
    std::string name;
    for (const char *c = name_utf8; *c; c++)
        if (!strchr("\\/:*?\"<>|", *c) && (unsigned char)*c >= 32) name += *c;
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (name.empty()) name = "MHXX Recomp";
    wchar_t wname[256], exe[1024], dir[1024], icon[1024];
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname, 256);
    GetModuleFileNameW(NULL, exe, 1024);
    wcscpy(dir, exe);
    if (wchar_t *slash = wcsrchr(dir, L'\\')) *slash = 0;
    swprintf(icon, 1024, L"%ls\\mhgurecomp.exe", dir);
    PWSTR desktop = NULL;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, NULL, &desktop))) {
        error = "desktop folder not found";
        return false;
    }
    wchar_t lnk[1024];
    swprintf(lnk, 1024, L"%ls\\%ls.lnk", desktop, wname);
    CoTaskMemFree(desktop);
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IShellLinkW *sl = NULL;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&sl);
    if (SUCCEEDED(hr)) {
        sl->SetPath(exe);
        sl->SetWorkingDirectory(dir);
        sl->SetIconLocation(icon, 0);
        sl->SetDescription(L"MHXX Recomp launcher");
        IPersistFile *pf = NULL;
        hr = sl->QueryInterface(IID_IPersistFile, (void **)&pf);
        if (SUCCEEDED(hr)) {
            hr = pf->Save(lnk, TRUE);
            pf->Release();
        }
        sl->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (FAILED(hr)) {
        char msg[64];
        SDL_snprintf(msg, sizeof msg, "error %#lx", (unsigned long)hr);
        error = msg;
        return false;
    }
    return true;
}
#else
static bool make_shortcut(const char *, std::string &error) {
    error = "only on Windows";
    return false;
}
#endif

// ---------------------------------------------------------------- the look (MHXX's menus: deep navy, gold trim,
// amber for what matters)
#define NAVY_TOP IM_COL32(14, 32, 78, 255)
#define NAVY_BOTTOM IM_COL32(4, 10, 28, 255)
#define GOLD IM_COL32(214, 172, 86, 255)
#define GOLD_DARK IM_COL32(120, 88, 34, 255)
static const ImVec4 GOLD_TEXT(.95f, .82f, .52f, 1), DIM_TEXT(.62f, .68f, .8f, 1);

// a double gold line round a panel, with small diamonds at its corners
static void gold_frame(ImDrawList *d, ImVec2 a, ImVec2 b, float s) {
    d->AddRect(a, b, GOLD_DARK, 6 * s, 0, 3 * s);
    d->AddRect(ImVec2(a.x + 4 * s, a.y + 4 * s), ImVec2(b.x - 4 * s, b.y - 4 * s), GOLD, 4 * s, 0, 1.2f * s);
    const ImVec2 corners[4] = {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)};
    for (const ImVec2 &c : corners) {
        float r = 6 * s;
        d->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), GOLD);
    }
}
// "name ........ value" with a gold diamond before it (value red when !good)
static void status_line(ImDrawList *d, const char *name, const char *value, bool good, float s) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float lh = ImGui::GetTextLineHeight(), r = 3.5f * s, cy = p.y + lh * .5f;
    d->AddQuadFilled(ImVec2(p.x + r, cy - r), ImVec2(p.x + 2 * r, cy), ImVec2(p.x + r, cy + r), ImVec2(p.x, cy), GOLD);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 14 * s, p.y));
    ImGui::TextColored(ImVec4(.8f, .84f, .92f, 1), "%s", name);
    float vw = ImGui::CalcTextSize(value).x;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - vw);
    ImGui::TextColored(good ? GOLD_TEXT : ImVec4(1, .55f, .4f, 1), "%s", value);
}
// a thin gold rule with a diamond in its middle
static void divider(ImDrawList *d, float width, float s) {
    ImGui::Dummy(ImVec2(0, 6 * s));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float y = p.y + 4 * s, mid = p.x + width * .5f, r = 4 * s;
    d->AddLine(ImVec2(p.x, y), ImVec2(mid - r * 2, y), GOLD_DARK, 1.5f * s);
    d->AddLine(ImVec2(mid + r * 2, y), ImVec2(p.x + width, y), GOLD_DARK, 1.5f * s);
    d->AddQuadFilled(ImVec2(mid, y - r), ImVec2(mid + r, y), ImVec2(mid, y + r), ImVec2(mid - r, y), GOLD);
    ImGui::Dummy(ImVec2(width, 14 * s));
}
// MHXX-like buttons: amber with a gold edge (primary), else navy with a gold edge; brighter under the mouse
static bool hunter_button(const char *id, const char *label, ImVec2 size, bool primary, bool enabled, float s) {
    ImGui::PushID(id);
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
    bool pressed = ImGui::InvisibleButton("##b", size) && enabled;
    bool hover = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
    ImDrawList *d = ImGui::GetWindowDrawList();
    float r = 5 * s;
    ImU32 top, bottom, text;
    if (!enabled) top = IM_COL32(50, 54, 64, 230), bottom = IM_COL32(32, 34, 42, 230), text = IM_COL32(130, 132, 140, 255);
    else if (primary)
        top = held ? IM_COL32(200, 120, 20, 255) : hover ? IM_COL32(255, 196, 80, 255) : IM_COL32(242, 172, 52, 255),
        bottom = held ? IM_COL32(150, 80, 10, 255) : hover ? IM_COL32(214, 128, 28, 255) : IM_COL32(184, 104, 18, 255),
        text = IM_COL32(28, 18, 6, 255);
    else
        top = hover ? IM_COL32(40, 70, 140, 240) : IM_COL32(24, 44, 96, 235),
        bottom = hover ? IM_COL32(18, 34, 80, 240) : IM_COL32(10, 22, 54, 235), text = IM_COL32(240, 226, 190, 255);
    if (hover && primary) d->AddRectFilled(ImVec2(a.x - 4 * s, a.y - 4 * s), ImVec2(b.x + 4 * s, b.y + 4 * s),
                                           IM_COL32(255, 190, 70, 60), r + 4 * s);
    d->AddRectFilledMultiColor(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), top, top, bottom, bottom);
    d->AddRect(a, b, enabled ? GOLD : IM_COL32(90, 90, 96, 255), r, 0, 2 * s);
    d->AddLine(ImVec2(a.x + 6 * s, a.y + 3 * s), ImVec2(b.x - 6 * s, a.y + 3 * s), IM_COL32(255, 255, 255, primary ? 90 : 40), 1 * s);
    float fs = ImGui::GetFontSize() * (primary ? 1.35f : 1.05f);
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, label);
    ImVec2 tp(a.x + (size.x - ts.x) * .5f, a.y + (size.y - ts.y) * .5f);
    if (primary && enabled) d->AddText(ImGui::GetFont(), fs, ImVec2(tp.x + 1, tp.y + 1), IM_COL32(255, 230, 170, 120), label);
    d->AddText(ImGui::GetFont(), fs, tp, text, label);
    ImGui::PopID();
    return pressed;
}

// the title screen, soft, covering the window (16:9, centred)
static void draw_background(SDL_Renderer *rd, const TitleArt &art, int w, int h) {
    if (!art.background) return;
    float sc = std::max((float)w / 1920.f, (float)h / 1080.f);
    SDL_FRect r = {(w - 1920 * sc) * .5f, (h - 1080 * sc) * .5f, 1920 * sc, 1080 * sc};
    SDL_RenderTexture(rd, art.background, nullptr, &r);
}

// Is the game running? (it holds the mutex Local\MHXXRecompRunning, runtime/ui.c)
static bool game_running() {
#ifdef _WIN32
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\MHXXRecompRunning");
    if (!m) return false;
    CloseHandle(m);
    return true;
#else
    return false;
#endif
}

// A dark veil over everything that takes the mouse (behind a dialog, or while the game runs)
static void veil(float w, float h, const char *id) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, .55f));
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::End();
    ImGui::PopStyleColor();
}

static bool start_game(std::string &error) {
    const char *base = SDL_GetBasePath();
    std::string exe = std::string(base ? base : "") + "mhgurecomp"
#ifdef _WIN32
                      + ".exe"
#endif
        ;
    const char *args[] = {exe.c_str(), nullptr};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!p) {
        error = SDL_GetError();
        return false;
    }
    SDL_DestroyProcess(p);
    return true;
}

// ---------------------------------------------------------------- the setup: the game files from the player's own
static std::mutex g_pick_lock;
static std::string g_picked_file[4];  // (the file dialogs' answers, from whatever thread SDL calls them on)
static void SDLCALL file_picked(void *slot, const char *const *list, int) {
    if (!list || !list[0]) return;
    std::lock_guard<std::mutex> g(g_pick_lock);
    g_picked_file[(intptr_t)slot] = list[0];
}
struct Setup {
    std::string path[4];  // the game, the update, the keys, the Korean patch
    bool keys_found = false;
    InstallJob *job = nullptr;
    std::thread th;
    std::string error;
    bool installed = false;
    void begin() {
        if (path[2].empty()) path[2] = install_find_keys(), keys_found = !path[2].empty();
        error.clear(), installed = false;
    }
    void poll() {
        std::lock_guard<std::mutex> g(g_pick_lock);
        for (int i = 0; i < 4; i++)
            if (!g_picked_file[i].empty()) {
                path[i] = g_picked_file[i], g_picked_file[i].clear();
                if (i == 2) keys_found = false;
            }
        if (job && job->finished) {
            th.join();
            if (job->ok) installed = true;
            else {
                std::lock_guard<std::mutex> j(job->lock);
                error = job->error;
            }
            delete job;
            job = nullptr;
        }
    }
    void stop() {  // (the window closed while it installs)
        if (!job) return;
        job->cancel = true;
        th.join();
        delete job;
        job = nullptr;
    }
};
static std::string file_name(const std::string &p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}
static std::string gb(uint64_t n) {
    char b[32];
    SDL_snprintf(b, sizeof b, "%.2f GB", n / 1073741824.0);
    return b;
}
// The setup screen: 0 still on it, 1 installed, 2 later (to the launcher without them)
static int setup_screen(Setup &st, SDL_Window *win, int w, int h, float s, bool have_game) {
    st.poll();
    float pad = 18 * s, dw = std::min(820 * s, (float)w - pad * 2);
    ImGui::SetNextWindowPos(ImVec2(w * .5f, h * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(dw, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30 * s, 26 * s));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.03f, .07f, .17f, .95f));
    ImGui::Begin("##setup", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImDrawList *d = ImGui::GetWindowDrawList();
    float cw = ImGui::GetContentRegionAvail().x;
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextColored(GOLD_TEXT, "%s", tr("Game file setup"));
    ImGui::SetWindowFontScale(1.0f);
    divider(d, cw, s);
    int result = 0;
    float bh = ImGui::GetFrameHeight() * 1.6f;
    if (st.job) {  // installing
        int stage = st.job->stage;
        static const char *names[] = {"",
                                      "Reading the keys",
                                      "Reading the game",
                                      "Reading the update",
                                      "Writing the ExeFS",
                                      "Merging the update into the RomFS",
                                      "Unpacking the Korean patch",
                                      "Putting the files in place",
                                      "Installed"};
        ImGui::TextColored(GOLD_TEXT, "%s", tr(names[std::clamp(stage, 0, 8)]));
        uint64_t done = st.job->done, total = st.job->total;
        float f = total ? std::min(1.f, (float)((double)done / total)) : 0;
        ImGui::Dummy(ImVec2(0, 6 * s));
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + 26 * s);
        d->AddRectFilled(a, b, IM_COL32(8, 14, 34, 255), 4 * s);
        if (f > 0)
            d->AddRectFilledMultiColor(ImVec2(a.x + 2, a.y + 2), ImVec2(a.x + 2 + (cw - 4) * f, b.y - 2), IM_COL32(255, 200, 90, 255),
                                       IM_COL32(255, 200, 90, 255), IM_COL32(190, 110, 20, 255), IM_COL32(190, 110, 20, 255));
        d->AddRect(a, b, GOLD, 4 * s, 0, 1.5f * s);
        ImGui::Dummy(ImVec2(cw, 26 * s));
        if (total) ImGui::TextColored(DIM_TEXT, "%d%%  %s / %s", (int)(f * 100), gb(done).c_str(), gb(total).c_str());
        std::string cur;
        {
            std::lock_guard<std::mutex> g(st.job->lock);
            cur = st.job->current;
        }
        if (!cur.empty()) ImGui::TextColored(DIM_TEXT, "%s", cur.c_str());
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::TextWrapped("%s", tr("Don't close the launcher until it is done."));
    } else if (st.installed) {
        ImGui::TextWrapped("%s", tr("The game files are in place."));
        ImGui::Dummy(ImVec2(0, 10 * s));
        if (hunter_button("setup.ok", tr("OK"), ImVec2(cw, bh), true, true, s) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown))
            result = 1;
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.88f, .9f, .95f, 1));
        ImGui::TextWrapped("%s", tr("Pick the files of your own Switch: the game, its 1.5.1 update and your console's keys. The launcher decrypts them and puts the game files in place. Nothing of the game comes with this program."));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8 * s));
        static const char *labels[] = {"Game (1.0.0, NSP or XCI)", "Update (1.5.1, NSP)", "Keys (prod.keys)",
                                       "Korean patch (zip, optional)"};
        static const SDL_DialogFileFilter game_f[] = {{"NSP / XCI", "nsp;xci"}}, upd_f[] = {{"NSP", "nsp"}},
                                          keys_f[] = {{"prod.keys", "keys"}}, zip_f[] = {{"zip", "zip"}};
        static const SDL_DialogFileFilter *filters[] = {game_f, upd_f, keys_f, zip_f};
        float bw = 130 * s;
        for (int i = 0; i < 4; i++) {
            ImGui::PushID(i);
            ImVec2 row = ImGui::GetCursorScreenPos();
            float rh = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
            d->AddRectFilled(row, ImVec2(row.x + cw, row.y + rh), IM_COL32(255, 255, 255, 8), 4 * s);
            float r = 3.5f * s, cy = row.y + ImGui::GetTextLineHeight() * .5f + 4 * s;
            d->AddQuadFilled(ImVec2(row.x + 8 * s + r, cy - r), ImVec2(row.x + 8 * s + 2 * r, cy),
                             ImVec2(row.x + 8 * s + r, cy + r), ImVec2(row.x + 8 * s, cy), GOLD);
            ImGui::SetCursorScreenPos(ImVec2(row.x + 22 * s, row.y + 4 * s));
            ImGui::TextColored(GOLD_TEXT, "%s", tr(labels[i]));
            ImGui::SetCursorScreenPos(ImVec2(row.x + 22 * s, ImGui::GetCursorScreenPos().y));
            std::string shown = st.path[i].empty() ? tr("Not chosen") : file_name(st.path[i]);
            if (i == 2 && st.keys_found) shown += std::string("  (") + tr("found in an emulator's folder") + ")";
            ImGui::PushTextWrapPos(row.x + cw - bw - 16 * s);
            ImGui::TextColored(st.path[i].empty() ? ImVec4(.6f, .64f, .72f, 1) : ImVec4(.92f, .93f, .96f, 1), "%s", shown.c_str());
            ImGui::PopTextWrapPos();
            if (!st.path[i].empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.path[i].c_str());
            ImGui::SetCursorScreenPos(ImVec2(row.x + cw - bw - 6 * s, row.y + (rh - ImGui::GetFrameHeight() * 1.4f) * .5f));
            if (hunter_button("browse", tr("Browse"), ImVec2(bw, ImGui::GetFrameHeight() * 1.4f), false, true, s))
                SDL_ShowOpenFileDialog(file_picked, (void *)(intptr_t)i, win, filters[i], 1, nullptr, false);
            ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + rh + 6 * s));
            ImGui::Dummy(ImVec2(cw, 0));
            ImGui::PopID();
        }
        if (have_game) ImGui::TextColored(DIM_TEXT, "%s", tr("The game files already here will be replaced."));
        if (!st.error.empty()) {
            char msg[1024];
            SDL_snprintf(msg, sizeof msg, tr("Could not install: %s"), st.error.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .55f, .4f, 1));
            ImGui::TextWrapped("%s", msg);
            ImGui::PopStyleColor();
            ImGui::TextColored(DIM_TEXT, "%s", tr("The details are in install.log."));
        }
        ImGui::Dummy(ImVec2(0, 8 * s));
        bool can = !st.path[0].empty() && !st.path[1].empty() && !st.path[2].empty();
        float b1 = (cw - 12 * s) * .62f, b2 = cw - 12 * s - b1;
        if (hunter_button("setup.install", tr("Install"), ImVec2(b1, bh), true, can, s)) {
            st.error.clear();
            st.job = new InstallJob;
            st.job->base = st.path[0], st.job->update = st.path[1], st.job->keys = st.path[2], st.job->patch = st.path[3];
            st.job->game_dir = g_game_dir;
            st.th = std::thread(install_run, st.job);
        }
        ImGui::SameLine(0, 12 * s);
        if (hunter_button("setup.later", tr(have_game ? "Cancel" : "Later"), ImVec2(b2, bh), false, true, s)) result = 2;
    }
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    gold_frame(ImGui::GetForegroundDrawList(), wp, ImVec2(wp.x + ws.x, wp.y + ws.y), s);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return result;
}

// ---------------------------------------------------------------- high-resolution textures (texmake.cpp)
struct TexMaker {
    bool gear = true, monsters = true;
    TexJob *job = nullptr;
    std::thread th;
    std::string error;
    bool made = false;
    int pack = 0;
    void begin() { error.clear(), made = false, pack = texmake_pack_count(); }
    void poll() {
        if (job && job->finished) {
            th.join();
            if (job->ok) made = true;
            else {
                std::lock_guard<std::mutex> j(job->lock);
                if (job->error != "cancelled") error = job->error;
            }
            delete job;
            job = nullptr;
            pack = texmake_pack_count();
        }
    }
    void stop() {
        if (!job) return;
        job->cancel = true;
        th.join();
        delete job;
        job = nullptr;
    }
};
static void check(const char *id, const char *label, bool *v, float s) {  // (a row with a gold box, ticked or not)
    ImGui::PushID(id);
    ImVec2 a = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight() + 12 * s, w = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton("##c", ImVec2(w, h))) *v = !*v;
    bool hover = ImGui::IsItemHovered();
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(a, ImVec2(a.x + w, a.y + h), hover ? IM_COL32(120, 160, 255, 28) : IM_COL32(255, 255, 255, 8), 4 * s);
    float bx = a.x + 10 * s, by = a.y + h * .5f, r = 8 * s;
    d->AddRect(ImVec2(bx, by - r), ImVec2(bx + 2 * r, by + r), GOLD, 3 * s, 0, 1.5f * s);
    if (*v) d->AddRectFilled(ImVec2(bx + 4 * s, by - r + 4 * s), ImVec2(bx + 2 * r - 4 * s, by + r - 4 * s), GOLD, 2 * s);
    d->AddText(ImVec2(bx + 2 * r + 12 * s, by - ImGui::GetTextLineHeight() * .5f), IM_COL32(242, 238, 228, 255), label);
    ImGui::PopID();
}
// The high-resolution textures screen: 0 still on it, 1 closed
static int tex_screen(TexMaker &tm, int w, int h, float s, bool have_game) {
    tm.poll();
    float pad = 18 * s, dw = std::min(820 * s, (float)w - pad * 2);
    ImGui::SetNextWindowPos(ImVec2(w * .5f, h * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(dw, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30 * s, 26 * s));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.03f, .07f, .17f, .95f));
    ImGui::Begin("##textures", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImDrawList *d = ImGui::GetWindowDrawList();
    float cw = ImGui::GetContentRegionAvail().x, bh = ImGui::GetFrameHeight() * 1.6f;
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextColored(GOLD_TEXT, "%s", tr("High-resolution textures"));
    ImGui::SetWindowFontScale(1.0f);
    divider(d, cw, s);
    int result = 0;
    if (tm.job) {
        int stage = tm.job->stage;
        uint64_t done = tm.job->done, total = tm.job->total;
        static const char *names[] = {"", "Taking the textures out of the game files", "Downloading the upscaler (Real-ESRGAN)",
                                      "Resizing the normal maps and masks", "Upscaling with AI on the graphics card", "Done"};
        ImGui::TextColored(GOLD_TEXT, "%s", tr(names[std::clamp(stage, 0, 5)]));
        float f = total ? std::min(1.f, (float)((double)done / total)) : 0;
        ImGui::Dummy(ImVec2(0, 6 * s));
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + 26 * s);
        d->AddRectFilled(a, b, IM_COL32(8, 14, 34, 255), 4 * s);
        if (!total) {  // (no end known: a moving band)
            float t = (float)fmod(SDL_GetTicks() / 1500.0, 1.0), x0 = a.x + (cw - cw * .25f) * t;
            d->AddRectFilled(ImVec2(x0, a.y + 2), ImVec2(x0 + cw * .25f, b.y - 2), IM_COL32(220, 150, 50, 255), 3 * s);
        } else if (f > 0)
            d->AddRectFilledMultiColor(ImVec2(a.x + 2, a.y + 2), ImVec2(a.x + 2 + (cw - 4) * f, b.y - 2), IM_COL32(255, 200, 90, 255),
                                       IM_COL32(255, 200, 90, 255), IM_COL32(190, 110, 20, 255), IM_COL32(190, 110, 20, 255));
        d->AddRect(a, b, GOLD, 4 * s, 0, 1.5f * s);
        ImGui::Dummy(ImVec2(cw, 26 * s));
        char line[160] = "";
        if (stage == TEX_DOWNLOAD) SDL_snprintf(line, sizeof line, "%.1f MB", done / 1048576.0);
        else if (!total) SDL_snprintf(line, sizeof line, tr("%d textures"), (int)done);
        else {
            SDL_snprintf(line, sizeof line, "%d / %d", (int)done, (int)total);
            uint64_t el = SDL_GetTicks() - tm.job->started;
            if (done > 0 && done < total && el > 5000) {
                int left = (int)((double)el / done * (total - done) / 60000) + 1;
                char more[96];
                SDL_snprintf(more, sizeof more, tr("  about %d min left"), left);
                SDL_strlcat(line, more, sizeof line);
            }
        }
        ImGui::TextColored(DIM_TEXT, "%s", line);
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::TextWrapped("%s", tr("You can stop it: what is made is kept, and the next time it goes on from there."));
        ImGui::Dummy(ImVec2(0, 6 * s));
        if (hunter_button("tex.stop", tr("Stop"), ImVec2(cw, bh), false, !tm.job->cancel, s)) tm.job->cancel = true;
    } else if (tm.made) {
        char msg[256];
        SDL_snprintf(msg, sizeof msg, tr("%d high-resolution textures are ready. They are used from the next start of the game."), tm.pack);
        ImGui::TextWrapped("%s", msg);
        ImGui::Dummy(ImVec2(0, 10 * s));
        if (hunter_button("tex.ok", tr("OK"), ImVec2(cw, bh), true, true, s)) result = 1;
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.88f, .9f, .95f, 1));
        ImGui::TextWrapped("%s", tr("Takes the textures out of your game files and upscales them on your graphics card with an AI model (Real-ESRGAN). The first time, the upscaler (about 40 MB) is downloaded from its GitHub page. It takes a while; you can stop it and go on later."));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8 * s));
        check("gear", tr("Armour, weapons and Palico gear (4x: 128 -> 512)"), &tm.gear, s);
        ImGui::Dummy(ImVec2(0, 2 * s));
        check("monsters", tr("Monsters (2x: 1024 -> 2048)"), &tm.monsters, s);
        ImGui::Dummy(ImVec2(0, 4 * s));
        char cur[128];
        SDL_snprintf(cur, sizeof cur, tr("Textures in the pack now: %d"), tm.pack);
        ImGui::TextColored(DIM_TEXT, "%s", cur);
        if (!have_game) ImGui::TextColored(ImVec4(1, .55f, .4f, 1), "%s", tr("Install the game files first."));
        if (!tm.error.empty()) {
            char msg[1024];
            SDL_snprintf(msg, sizeof msg, tr("Could not make them: %s"), tm.error.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .55f, .4f, 1));
            ImGui::TextWrapped("%s", msg);
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 8 * s));
        float b1 = (cw - 12 * s) * .62f, b2 = cw - 12 * s - b1;
        if (hunter_button("tex.make", tr("Make"), ImVec2(b1, bh), true, have_game && (tm.gear || tm.monsters), s)) {
            tm.error.clear();
            tm.job = new TexJob;
            tm.job->gear = tm.gear, tm.job->monsters = tm.monsters, tm.job->game_dir = g_game_dir;
            tm.th = std::thread(texmake_run, tm.job);
        }
        ImGui::SameLine(0, 12 * s);
        if (hunter_button("tex.close", tr("Close"), ImVec2(b2, bh), false, true, s)) result = 1;
    }
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    gold_frame(ImGui::GetForegroundDrawList(), wp, ImVec2(wp.x + ws.x, wp.y + ws.y), s);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return result;
}

int main(int argc, char **argv) {
    (void)argc, (void)argv;
    {  // (the game's folder: its settings, data and mods are beside the programs)
        const char *base = SDL_GetBasePath();
        if (base && chdir(base) != 0) SDL_Log("can't enter %s", base);
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MHXX Recomp", SDL_GetError(), nullptr);
        return 1;
    }
    config_load();
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;
    SDL_Window *win = nullptr;
    SDL_Renderer *rd = nullptr;
    std::string title = std::string("MHXX Recomp - ") + tr("Launcher");
    if (!SDL_CreateWindowAndRenderer(title.c_str(), (int)(1280 * scale), (int)(800 * scale),
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &win, &rd)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MHXX Recomp", SDL_GetError(), nullptr);
        return 1;
    }
    SDL_SetRenderVSync(rd, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImFontConfig cfg;
    cfg.SizePixels = 18 * scale;
    // (the system's Hangul font for everything - Malgun Gothic on Windows - else ImGui's own)
    const char *ko = korean_font();
    if (!ko || !io.Fonts->AddFontFromFileTTF(ko, 19 * scale)) io.Fonts->AddFontDefaultVector(&cfg);
    ImGui_ImplSDL3_InitForSDLRenderer(win, rd);
    ImGui_ImplSDLRenderer3_Init(rd);
    TitleArt art;
    title_art_load(rd, g_game_dir, &art);  // (from the player's own game files; none: the plain navy look)
    {
        ImGuiStyle &st = ImGui::GetStyle();  // (the dialogs and anything else ImGui draws: the same navy and gold)
        st.WindowRounding = 6 * scale, st.FrameRounding = 4 * scale, st.WindowBorderSize = 1.5f;
        st.Colors[ImGuiCol_WindowBg] = ImVec4(.04f, .08f, .19f, .94f);
        st.Colors[ImGuiCol_Border] = ImVec4(.84f, .67f, .34f, .9f);
        st.Colors[ImGuiCol_FrameBg] = ImVec4(.08f, .14f, .3f, 1);
        st.Colors[ImGuiCol_Button] = ImVec4(.1f, .18f, .38f, 1);
        st.Colors[ImGuiCol_ButtonHovered] = ImVec4(.85f, .6f, .2f, 1);
        st.Colors[ImGuiCol_ButtonActive] = ImVec4(.7f, .45f, .12f, 1);
        st.Colors[ImGuiCol_Text] = ImVec4(.95f, .93f, .88f, 1);
        st.Colors[ImGuiCol_NavCursor] = ImVec4(.95f, .75f, .35f, 1);
        st.Colors[ImGuiCol_TextSelectedBg] = ImVec4(.85f, .6f, .2f, .45f);
    }

    char exefs[64];
    SDL_snprintf(exefs, sizeof exefs, "%s/exefs/main", g_game_dir);
    bool have_game = SDL_GetPathInfo(exefs, nullptr);
    Setup setup;
    TexMaker texmaker;
    bool in_tex = SDL_getenv("MHXX_LAUNCHER_SHOT") && SDL_getenv("MHXX_LAUNCHER_SHOT_TEX");
    if (in_tex) texmaker.begin();
    bool in_setup = !have_game || (SDL_getenv("MHXX_LAUNCHER_SHOT") && SDL_getenv("MHXX_LAUNCHER_SHOT_SETUP"));
    if (in_setup) setup.begin();
    std::string error;
    bool open = true, quit = false, start = false;
    bool confirm_quit = SDL_getenv("MHXX_LAUNCHER_SHOT") && SDL_getenv("MHXX_LAUNCHER_SHOT_QUIT"), running = false;
    uint64_t running_checked = 0;
    std::string mods_text, pack_text;  // (the left column's summary: counted at the start and after the setup)
    auto count = [&]() {
        int n = 0, mods = 0, pngs = 0;
        char **e = SDL_GlobDirectory("mods", "*", 0, &n);
        for (int i = 0; e && i < n; i++)
            if (!strchr(e[i], '/') && !strchr(e[i], '\\')) {
                SDL_PathInfo info;
                std::string p = std::string("mods/") + e[i];
                if (SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY) mods++;
            }
        SDL_free(e);
        e = SDL_GlobDirectory("textures/load", nullptr, 0, &n);
        for (int i = 0; e && i < n; i++) {
            size_t l = strlen(e[i]);
            if (l > 4 && SDL_strcasecmp(e[i] + l - 4, ".png") == 0) pngs++;
        }
        SDL_free(e);
        char buf[64];
        SDL_snprintf(buf, sizeof buf, tr("%d installed"), mods);
        mods_text = mods ? buf : tr("None");
        SDL_snprintf(buf, sizeof buf, tr("%d textures"), pngs);
        pack_text = pngs ? buf : tr("None");
    };
    count();
#ifdef _WIN32
    bool ask_shortcut = !shortcut_asked();
#else
    bool ask_shortcut = false;
#endif
    char shortcut_name[128] = "MHXX Recomp";
    const char *shot = SDL_getenv("MHXX_LAUNCHER_SHOT");
    int frames = 0;
    if (shot) ask_shortcut = SDL_getenv("MHXX_LAUNCHER_SHOT_ASK") != nullptr;
    bool name_focus = true;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win))) {
                if (in_setup || in_tex) quit = true;  // (an install going on stops: what it wrote so far is removed)
                else if (!ask_shortcut && !running && settings_panel_dirty()) confirm_quit = true;  // (unsaved: ask)
                else quit = true;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !ask_shortcut && !confirm_quit && !running && !in_setup && !in_tex)
                settings_panel_key(e.key.scancode);
        }
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(50);
            continue;
        }
        if (SDL_SetAtomicInt(&want_folder, 0))
            SDL_ShowOpenFolderDialog(folder_picked, nullptr, win,
                                     g_config.save_location[0] ? g_config.save_location : nullptr, false);
        int w, h;
        SDL_GetWindowSize(win, &w, &h);
        float pad = 18 * scale, side = std::clamp(w * .34f, 340 * scale, 460 * scale);
        settings_panel_set_launcher(side + pad, pad, w - side - pad * 2, h - pad * 2);
        open = true;
        if (SDL_GetTicks() - running_checked > 1000 || !running_checked) {  // (once a second)
            running_checked = SDL_GetTicks();
            running = game_running();
        }
        if (!ask_shortcut && !confirm_quit && !running && !in_setup && !in_tex) settings_panel_before_frame(open);
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        if (ask_shortcut) {  // the first start: a desktop shortcut? (its name can be changed)
            float dw = std::min(620 * scale, (float)w - pad * 2);
            ImGui::SetNextWindowPos(ImVec2(w * .5f, h * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
            ImGui::SetNextWindowSize(ImVec2(dw, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * scale, 24 * scale));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.03f, .07f, .17f, .95f));
            ImGui::Begin("##shortcut", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize);
            ImDrawList *dd = ImGui::GetWindowDrawList();
            float cw2 = ImGui::GetContentRegionAvail().x;
            ImGui::SetWindowFontScale(1.2f);
            ImGui::TextColored(GOLD_TEXT, "%s", tr("Make a shortcut to the launcher on the desktop?"));
            ImGui::SetWindowFontScale(1.0f);
            divider(dd, cw2, scale);
            ImGui::TextColored(DIM_TEXT, "%s", tr("Shortcut name"));
            ImGui::SetNextItemWidth(-1);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * scale, 8 * scale));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(.07f, .13f, .28f, 1));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.84f, .67f, .34f, 1));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.2f * scale);
            if (name_focus) ImGui::SetKeyboardFocusHere(), name_focus = false;
            bool enter = ImGui::InputText("##name", shortcut_name, sizeof shortcut_name,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            if (!error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .55f, .4f, 1));
                ImGui::TextWrapped("%s", error.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 10 * scale));
            float bw2 = (cw2 - 12 * scale) * .5f, bh2 = ImGui::GetFrameHeight() * 1.7f;
            bool yes = hunter_button("yes", tr("Yes"), ImVec2(bw2, bh2), true, true, scale) || enter;
            ImGui::SameLine(0, 12 * scale);
            bool no = hunter_button("no", tr("No"), ImVec2(bw2, bh2), false, true, scale);
            ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            gold_frame(ImGui::GetForegroundDrawList(), wp, ImVec2(wp.x + ws.x, wp.y + ws.y), scale);
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            if (yes) {
                std::string why;
                if (make_shortcut(shortcut_name, why)) {
                    shortcut_remember();
                    ask_shortcut = false;
                    error.clear();
                } else {
                    char msg[256];
                    SDL_snprintf(msg, sizeof msg, tr("Could not make the shortcut: %s"), why.c_str());
                    error = msg;
                }
            } else if (no) {
                shortcut_remember();
                ask_shortcut = false;
                error.clear();
            }
            ImGui::Render();
            SDL_SetRenderScale(rd, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(rd, 4, 10, 28, 255);
            SDL_RenderClear(rd);
            draw_background(rd, art, w, h);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), rd);
            if (shot && ++frames == 20) {
                if (SDL_Surface *sf = SDL_RenderReadPixels(rd, nullptr)) SDL_SavePNG(sf, shot), SDL_DestroySurface(sf);
                quit = true;
            }
            SDL_RenderPresent(rd);
            continue;
        }

        if (in_tex) {  // high-resolution textures
            if (!art.background)
                ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2((float)w, (float)h), NAVY_TOP,
                                                                         NAVY_TOP, NAVY_BOTTOM, NAVY_BOTTOM);
            if (tex_screen(texmaker, w, h, scale, have_game)) in_tex = false, count();
            ImGui::Render();
            SDL_SetRenderScale(rd, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(rd, 4, 10, 28, 255);
            SDL_RenderClear(rd);
            draw_background(rd, art, w, h);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), rd);
            if (shot && ++frames == 20) {
                if (SDL_Surface *sf = SDL_RenderReadPixels(rd, nullptr)) SDL_SavePNG(sf, shot), SDL_DestroySurface(sf);
                quit = true;
            }
            SDL_RenderPresent(rd);
            continue;
        }
        if (in_setup) {  // the game files first (none yet, or "Install game files")
            if (!art.background)
                ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2((float)w, (float)h), NAVY_TOP,
                                                                         NAVY_TOP, NAVY_BOTTOM, NAVY_BOTTOM);
            int r = setup_screen(setup, win, w, h, scale, have_game);
            if (r) {
                in_setup = false;
                if (r == 1) {  // (installed: the game's own title screen behind the launcher now, the mods counted again)
                    have_game = SDL_GetPathInfo(exefs, nullptr);
                    for (SDL_Texture *t : {art.background, art.logo, art.emblem})
                        if (t) SDL_DestroyTexture(t);
                    title_art_load(rd, g_game_dir, &art);
                    count();
                    error.clear();
                }
            }
            ImGui::Render();
            SDL_SetRenderScale(rd, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(rd, 4, 10, 28, 255);
            SDL_RenderClear(rd);
            draw_background(rd, art, w, h);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), rd);
            if (shot && ++frames == 20) {
                if (SDL_Surface *sf = SDL_RenderReadPixels(rd, nullptr)) SDL_SavePNG(sf, shot), SDL_DestroySurface(sf);
                quit = true;
            }
            SDL_RenderPresent(rd);
            continue;
        }

        // ---- the look: the title screen behind everything, a hunter's notice board on the left
        ImDrawList *bg = ImGui::GetBackgroundDrawList();
        ImVec2 W((float)w, (float)h);
        if (!art.background) {  // (with the picture: drawn by SDL before ImGui, draw_background)
            bg->AddRectFilledMultiColor(ImVec2(0, 0), W, NAVY_TOP, NAVY_TOP, NAVY_BOTTOM, NAVY_BOTTOM);
        }
        bg->AddRectFilledMultiColor(ImVec2(0, W.y * .55f), W, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                    IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150));
        ImVec2 c0(pad, pad), c1(side, W.y - pad);  // the left column
        bg->AddRectFilled(c0, c1, IM_COL32(8, 16, 38, 205), 6 * scale);
        if (art.emblem) {  // (the title menu's stone emblem, faint, at the column's foot)
            float ew = (c1.x - c0.x) * .9f;
            ImVec2 e0((c0.x + c1.x - ew) * .5f, c1.y - ew - 20 * scale);
            bg->AddImage((ImTextureID)(intptr_t)art.emblem, e0, ImVec2(e0.x + ew, e0.y + ew), ImVec2(0, 0),
                         ImVec2(1, 1), IM_COL32(255, 255, 255, 46));
        }
        gold_frame(bg, c0, c1, scale);

        ImGui::SetNextWindowPos(ImVec2(c0.x + 14 * scale, c0.y + 10 * scale));
        ImGui::SetNextWindowSize(ImVec2(c1.x - c0.x - 28 * scale, c1.y - c0.y - 20 * scale));
        ImGui::Begin("##launcher", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float cw = ImGui::GetContentRegionAvail().x;
        if (art.logo) {
            float lh = cw * art.logo_h / art.logo_w;
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddImage((ImTextureID)(intptr_t)art.logo, p, ImVec2(p.x + cw, p.y + lh));
            ImGui::Dummy(ImVec2(cw, lh));
        } else {
            ImGui::SetWindowFontScale(1.6f);
            ImGui::TextColored(GOLD_TEXT, "MONSTER HUNTER XX");
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::TextColored(GOLD_TEXT, "Nintendo Switch Ver. 1.5.1");
        ImGui::TextColored(DIM_TEXT, "MHXX Recomp");
        divider(dl, cw, scale);
        status_line(dl, tr("Game files"), have_game ? tr("Found") : tr("Missing"), have_game, scale);
        status_line(dl, tr("Game"), running ? tr("Running") : tr("Not running"), !running, scale);
        status_line(dl, tr("Mods"), mods_text.c_str(), true, scale);
        status_line(dl, tr("Texture pack"), pack_text.c_str(), true, scale);
        divider(dl, cw, scale);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.88f, .9f, .95f, 1));
        ImGui::TextWrapped("%s", tr("Settings applied when the game starts. Everything else is in the game's settings menu (F1)."));
        ImGui::PopStyleColor();
        if (!have_game) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .55f, .4f, 1));
            ImGui::TextWrapped("%s", tr("Game files not found: put them in with Install game files."));
            ImGui::PopStyleColor();
        }
        float big = ImGui::GetFrameHeight() * 2.4f, small = ImGui::GetFrameHeight() * 1.5f, gap = 10 * scale;
        float y = ImGui::GetWindowHeight() - big - small * 2 - gap * 2 - ImGui::GetStyle().WindowPadding.y * 2;
        if (!error.empty()) {
            ImGui::SetCursorPosY(y - ImGui::GetTextLineHeightWithSpacing() * 2.2f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .55f, .4f, 1));
            ImGui::TextWrapped("%s", error.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::SetCursorPosY(y);
        bool can_start = have_game && !running && !confirm_quit;
        if (hunter_button("start", tr(running ? "Game is running" : "Start game"), ImVec2(cw, big), true, can_start, scale))
            start = true;
        ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
        float half = (cw - gap) * .5f;  // (two side by side: the game files, the textures)
        if (hunter_button("setup", tr("Install game files"), ImVec2(half, small), false, !running && !confirm_quit, scale)) {
            if (settings_panel_dirty()) settings_panel_save_now();  // (the settings changed here: kept)
            in_setup = true;
            setup.begin();
        }
        ImGui::SameLine(0, gap);
        if (hunter_button("textures", tr("High-resolution textures"), ImVec2(half, small), false, !running && !confirm_quit, scale)) {
            if (settings_panel_dirty()) settings_panel_save_now();
            in_tex = true;
            texmaker.begin();
        }
        ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
        if (hunter_button("quit", tr("Quit"), ImVec2(cw, small), false, !confirm_quit, scale)) {
            if (!running && settings_panel_dirty()) confirm_quit = true;
            else quit = true;
        }
        ImGui::End();
        if (can_start && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) start = true;  // (+ on the controller)

        if (!running) settings_panel(&open);  // (the right: the settings, launcher mode; not while the game runs)

        if (running) {  // the game is running: its settings are its own until it closes
            veil((float)w, (float)h, "##veil_running");
            ImGui::SetNextWindowPos(ImVec2(side + pad + (w - side - pad * 2) * .5f, h * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
            ImGui::SetNextWindowSize(ImVec2(std::min(520 * scale, (float)w - side - pad * 3), 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * scale, 24 * scale));
            ImGui::Begin("##running", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav);
            ImGui::SetWindowFontScale(1.2f);
            ImGui::TextColored(GOLD_TEXT, "%s", tr("The game is running"));
            ImGui::SetWindowFontScale(1.0f);
            divider(ImGui::GetWindowDrawList(), ImGui::GetContentRegionAvail().x, scale);
            ImGui::TextWrapped("%s", tr("Change these settings after closing the game (or in the game's settings menu, F1). The game saves its own settings and would overwrite changes made here."));
            ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            gold_frame(ImGui::GetForegroundDrawList(), wp, ImVec2(wp.x + ws.x, wp.y + ws.y), scale);
            ImGui::End();
            ImGui::PopStyleVar();
        }
        int quit_choice = -1;  // unsaved changes at Quit: 0 save and quit, 1 quit without saving, 2 cancel
        if (confirm_quit) {
            veil((float)w, (float)h, "##veil_quit");
            ImGui::SetNextWindowPos(ImVec2(w * .5f, h * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
            ImGui::SetNextWindowSize(ImVec2(std::min(640 * scale, (float)w - pad * 2), 0));
            ImGui::SetNextWindowFocus();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * scale, 24 * scale));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.03f, .07f, .17f, .97f));
            ImGui::Begin("##quit", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
            float qw = ImGui::GetContentRegionAvail().x;
            ImGui::SetWindowFontScale(1.2f);
            ImGui::TextColored(GOLD_TEXT, "%s", tr("There are unsaved changes. Save them?"));
            ImGui::SetWindowFontScale(1.0f);
            divider(ImGui::GetWindowDrawList(), qw, scale);
            float bw3 = (qw - 24 * scale) / 3, bh3 = ImGui::GetFrameHeight() * 1.7f;
            if (hunter_button("q.save", tr("Save and quit"), ImVec2(bw3, bh3), true, true, scale)) quit_choice = 0;
            ImGui::SameLine(0, 12 * scale);
            if (hunter_button("q.discard", tr("Quit without saving"), ImVec2(bw3, bh3), false, true, scale)) quit_choice = 1;
            ImGui::SameLine(0, 12 * scale);
            if (hunter_button("q.cancel", tr("Cancel"), ImVec2(bw3, bh3), false, true, scale) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))
                quit_choice = 2;
            ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            gold_frame(ImGui::GetForegroundDrawList(), wp, ImVec2(wp.x + ws.x, wp.y + ws.y), scale);
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }
        if (quit_choice == 0) {
            if (settings_panel_save_now()) quit = true;
            else error = tr("Could not save the settings."), confirm_quit = false;
        } else if (quit_choice == 1) {
            quit = true;
        } else if (quit_choice == 2) {
            confirm_quit = false;
        }

        ImGui::Render();
        SDL_SetRenderScale(rd, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(rd, 4, 10, 28, 255);
        SDL_RenderClear(rd);
        draw_background(rd, art, w, h);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), rd);
        if (shot && ++frames == 20) {  // (MHXX_LAUNCHER_SHOT=<file.png>: a picture of the launcher, then quit)
            if (SDL_Surface *sf = SDL_RenderReadPixels(rd, nullptr)) {
                SDL_SavePNG(sf, shot);
                SDL_DestroySurface(sf);
            }
            quit = true;
        }
        SDL_RenderPresent(rd);

        if (start) {
            start = false;
            if (!settings_panel_save_now()) error = tr("Could not save the settings.");
            else if (!start_game(error)) {
                char msg[512];
                SDL_snprintf(msg, sizeof msg, tr("Could not start the game: %s"), error.c_str());
                error = msg;
            } else quit = true;
        }
    }
    setup.stop();
    texmaker.stop();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(rd);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
