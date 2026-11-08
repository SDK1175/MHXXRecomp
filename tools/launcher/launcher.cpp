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
#include <cfloat>
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
    std::string error;
    bool open = true, quit = false, start = false;
    std::string mods_text, pack_text;  // (the left column's summary: counted once)
    {
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
    }
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
        float pad = 18 * scale, side = std::clamp(w * .34f, 340 * scale, 460 * scale);
        settings_panel_set_launcher(side + pad, pad, w - side - pad * 2, h - pad * 2);
        open = true;
        if (!ask_shortcut) settings_panel_before_frame(open);
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
        status_line(dl, tr("Mods"), mods_text.c_str(), true, scale);
        status_line(dl, tr("Texture pack"), pack_text.c_str(), true, scale);
        divider(dl, cw, scale);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.88f, .9f, .95f, 1));
        ImGui::TextWrapped("%s", tr("Settings applied when the game starts. Everything else is in the game's settings menu (F1)."));
        ImGui::PopStyleColor();
        if (!have_game) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .55f, .4f, 1));
            ImGui::TextWrapped("%s", tr("Game files not found: put the game's exefs and romfs folders in the data folder."));
            ImGui::PopStyleColor();
        }
        float big = ImGui::GetFrameHeight() * 2.4f, small = ImGui::GetFrameHeight() * 1.5f, gap = 10 * scale;
        float y = ImGui::GetWindowHeight() - big - small - gap - ImGui::GetStyle().WindowPadding.y * 2;
        if (!error.empty()) {
            ImGui::SetCursorPosY(y - ImGui::GetTextLineHeightWithSpacing() * 2.2f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .55f, .4f, 1));
            ImGui::TextWrapped("%s", error.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::SetCursorPosY(y);
        if (hunter_button("start", tr("Start game"), ImVec2(cw, big), true, have_game, scale)) start = true;
        ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
        if (hunter_button("quit", tr("Quit"), ImVec2(cw, small), false, true, scale)) quit = true;
        ImGui::End();
        if (have_game && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) start = true;  // (+ on the controller)

        settings_panel(&open);  // (the right: the settings, launcher mode)

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
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(rd);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
