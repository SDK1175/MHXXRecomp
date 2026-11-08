// mhxx-launcher: the game's launcher. A window of its own (SDL's 2D renderer, no Vulkan) with the game's settings
// menu (runtime/settings_panel.cpp) showing only what takes effect when the game starts (window, resolution,
// render resolution, FSR, anisotropic filtering, the save's folder, mods ...), and Start game / Quit. Start game
// saves the settings and starts mhgurecomp next to it, which then reads them; the game's own Exit to launcher (F1)
// starts this again.
//
// Everything else stays in the game's menu (F1), which keeps these settings too.
#include <SDL3/SDL.h>

#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "app_actions.h"
#include "config.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "settings_locale.h"
#include "settings_panel.h"

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
    io.Fonts->AddFontDefaultVector(&cfg);
    if (const char *ko = korean_font()) {
        ImFontConfig merge;
        merge.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(ko, 18 * scale, &merge);
    }
    ImGui_ImplSDL3_InitForSDLRenderer(win, rd);
    ImGui_ImplSDLRenderer3_Init(rd);

    char exefs[64];
    SDL_snprintf(exefs, sizeof exefs, "%s/exefs/main", g_game_dir);
    bool have_game = SDL_GetPathInfo(exefs, nullptr);
    std::string error;
    bool open = true, quit = false, start = false;
#ifdef _WIN32
    bool ask_shortcut = !shortcut_asked();
#else
    bool ask_shortcut = false;
#endif
    char shortcut_name[128] = "MHXX Recomp";
    bool name_focus = true;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win)))
                quit = true;
            if (e.type == SDL_EVENT_KEY_DOWN && !ask_shortcut) settings_panel_key(e.key.scancode);
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
        float side = 300 * scale, pad = 16 * scale;
        settings_panel_set_launcher(side + pad, pad, w - side - pad * 2, h - pad * 2);
        open = true;
        if (!ask_shortcut) settings_panel_before_frame(open);
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        if (ask_shortcut) {  // the first start: a desktop shortcut? (its name can be changed)
            float dw = std::min(560 * scale, (float)w - pad * 2);
            ImGui::SetNextWindowPos(ImVec2(w * .5f, h * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
            ImGui::SetNextWindowSize(ImVec2(dw, 0));
            ImGui::Begin("##shortcut", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::TextWrapped("%s", tr("Make a shortcut to the launcher on the desktop?"));
            ImGui::Spacing();
            ImGui::TextUnformatted(tr("Shortcut name"));
            ImGui::SetNextItemWidth(-1);
            if (name_focus) ImGui::SetKeyboardFocusHere(), name_focus = false;
            bool enter = ImGui::InputText("##name", shortcut_name, sizeof shortcut_name,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
            if (!error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .55f, .4f, 1));
                ImGui::TextWrapped("%s", error.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
            float bw2 = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
            bool yes = ImGui::Button(tr("Yes"), ImVec2(bw2, ImGui::GetFrameHeight() * 1.6f)) || enter;
            ImGui::SameLine();
            bool no = ImGui::Button(tr("No"), ImVec2(bw2, ImGui::GetFrameHeight() * 1.6f));
            ImGui::End();
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
            SDL_SetRenderDrawColor(rd, 18, 20, 24, 255);
            SDL_RenderClear(rd);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), rd);
            SDL_RenderPresent(rd);
            continue;
        }

        // the left column: the game, Start game, Quit
        ImGui::SetNextWindowPos(ImVec2(pad, pad));
        ImGui::SetNextWindowSize(ImVec2(side - pad, h - pad * 2));
        ImGui::Begin("##launcher", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextWrapped("Monster Hunter XX");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("Nintendo Switch Ver. %s", "1.5.1");
        ImGui::TextDisabled("MHXX Recomp");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("%s", tr("Settings applied when the game starts. Everything else is in the game's settings menu (F1)."));
        ImGui::Spacing();
        if (!have_game) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .55f, .4f, 1));
            ImGui::TextWrapped("%s", tr("Game files not found: put the game's exefs and romfs folders in the data folder."));
            ImGui::PopStyleColor();
        }
        float bw = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - (ImGui::GetFrameHeight() * 2.6f + ImGui::GetFrameHeight() * 1.6f) -
                             ImGui::GetStyle().WindowPadding.y * 2);
        if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
        ImGui::BeginDisabled(!have_game);
        if (ImGui::Button(tr("Start game"), ImVec2(bw, ImGui::GetFrameHeight() * 2.6f))) start = true;
        ImGui::EndDisabled();
        if (ImGui::Button(tr("Quit"), ImVec2(bw, ImGui::GetFrameHeight() * 1.6f))) quit = true;
        ImGui::End();

        settings_panel(&open);  // (the right: the settings, launcher mode)

        ImGui::Render();
        SDL_SetRenderScale(rd, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(rd, 18, 20, 24, 255);
        SDL_RenderClear(rd);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), rd);
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
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(rd);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
