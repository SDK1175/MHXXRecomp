#pragma once
#include <SDL3/SDL.h>

// The launcher's pictures from the player's own game files (title_art.cpp). Each may be null (files not there).
struct TitleArt {
    SDL_Texture *background = nullptr;  // the title screen, soft and darker
    SDL_Texture *logo = nullptr;        // its logo
    int logo_w = 0, logo_h = 0;
    SDL_Texture *emblem = nullptr;  // the title menu's stone emblem
    int emblem_w = 0, emblem_h = 0;
};
bool title_art_load(SDL_Renderer *rd, const char *game_dir, TitleArt *art);
