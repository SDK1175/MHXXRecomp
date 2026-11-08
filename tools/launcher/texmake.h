#pragma once
// The launcher's "High-resolution textures" (texmake.cpp): a texture pack made on the player's own computer from
// their own game files - nothing of the game comes with the program. The textures are taken out of the game's .arc
// files (mhxx-texextract, beside the launcher), the colour ones upscaled by Real-ESRGAN's anime model on the graphics
// card (realesrgan-ncnn-vulkan: downloaded from its GitHub release the first time), the normal maps and masks
// resized smoothly, into textures/load where the game reads them. What is there already is kept, so it can be
// stopped and started again.
#include <atomic>
#include <mutex>
#include <string>

struct TexJob {
    bool gear = true;      // armour, weapons, Palico gear: 4x (128 -> 512)
    bool monsters = true;  // monsters: 2x (1024 -> 2048)
    std::string game_dir = "data", mods_dir = "mods";
    std::atomic<int> stage{0};  // TexStage
    std::atomic<uint64_t> done{0}, total{0};
    std::atomic<uint64_t> started{0};  // (SDL_GetTicks of the stage's start: the time left)
    std::atomic<bool> finished{false}, ok{false}, cancel{false};
    std::mutex lock;
    std::string error;
};
enum TexStage { TEX_EXTRACT = 1, TEX_DOWNLOAD, TEX_RESIZE, TEX_UPSCALE, TEX_DONE };
void texmake_run(TexJob *job);
// textures/load: how many pictures are in it
int texmake_pack_count();
