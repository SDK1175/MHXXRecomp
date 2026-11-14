// The controller's motion sensors (gyro, accelerometer) for the game's six-axis sensor (hid GetSixAxisSensorState:
// runtime/hle/input.c), from a controller that has them (DualShock 4, DualSense, Switch Pro, Joy-Con: SDL3 reads
// them over USB and Bluetooth). MHXX aims the bowguns and the bow with it when its own Gyro option is on.
//
// Read on the window thread each frame (host_input_update), the orientation integrated there, the state the game
// reads copied out under a lock. Without such a controller (an Xbox one, a keyboard), or [Controls] Gyro = Off, the
// state stays at rest: 1 g down, no rotation.
//
// Axes: SDL's (x right, y up, z towards the player; rad/s, m/s^2) to hid's as yuzu maps them (x right, y forward,
// z up; rotations a second, g): x = x, y = -z, z = y.
#include <math.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "config.h"
#include "rt.h"

typedef struct {
    float accel[3], gyro[3], angle[3], dir[9];
    bool rest;
} Motion;

static SDL_Mutex *g_lock;
static Motion g_now = {{0, 0, -1}, {0}, {0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, true};
static float q[4] = {1, 0, 0, 0};  // the orientation (w, x, y, z), from the gyro
static uint64_t g_last_ns;
static SDL_Gamepad *g_enabled;

static void rotation(const float *q4, float *m) {
    float w = q4[0], x = q4[1], y = q4[2], z = q4[3];
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y - w * z), m[2] = 2 * (x * z + w * y);
    m[3] = 2 * (x * y + w * z), m[4] = 1 - 2 * (x * x + z * z), m[5] = 2 * (y * z - w * x);
    m[6] = 2 * (x * z - w * y), m[7] = 2 * (y * z + w * x), m[8] = 1 - 2 * (x * x + y * y);
}

// Each frame, with the controller in use (NULL: none)
void motion_update(SDL_Gamepad *pad) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    Motion m = {{0, 0, -1}, {0}, {0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, true};
    uint64_t now = SDL_GetTicksNS();
    float dt = g_last_ns ? (float)(now - g_last_ns) / 1e9f : 0;
    g_last_ns = now;
    if (dt > 0.1f) dt = 0.1f;
    bool have = pad && g_config.gyro && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO);
    if (have && pad != g_enabled) {
        SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
        if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
        LOG("[input] motion sensors on: %s\n", SDL_GetGamepadName(pad));
        g_enabled = pad, q[0] = 1, q[1] = q[2] = q[3] = 0;
    }
    if (have) {
        float g[3] = {0}, a[3] = {0, -SDL_STANDARD_GRAVITY, 0};
        SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3);
        if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, a, 3);
        const float TAU = 6.2831853f;
        float gx = g[0], gy = -g[2], gz = g[1];  // rad/s, hid axes
        m.gyro[0] = gx / TAU, m.gyro[1] = gy / TAU, m.gyro[2] = gz / TAU;
        m.accel[0] = a[0] / SDL_STANDARD_GRAVITY, m.accel[1] = -a[2] / SDL_STANDARD_GRAVITY, m.accel[2] = a[1] / SDL_STANDARD_GRAVITY;
        // the orientation: q += q * (0, w) * dt / 2
        float hx = gx * dt * .5f, hy = gy * dt * .5f, hz = gz * dt * .5f;
        float w = q[0], x = q[1], y = q[2], z = q[3];
        q[0] += -x * hx - y * hy - z * hz, q[1] += w * hx + y * hz - z * hy;
        q[2] += w * hy - x * hz + z * hx, q[3] += w * hz + x * hy - y * hx;
        float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (n > 0) for (int i = 0; i < 4; i++) q[i] /= n;
        static float angle[3];
        for (int i = 0; i < 3; i++) angle[i] += m.gyro[i] * dt, m.angle[i] = angle[i];
        rotation(q, m.dir);
        m.rest = fabsf(gx) + fabsf(gy) + fabsf(gz) < 0.05f;
    } else if (g_enabled && g_enabled != pad) {
        g_enabled = NULL;
    }
    SDL_LockMutex(g_lock);
    g_now = m;
    SDL_UnlockMutex(g_lock);
}

// hid's SixAxisSensorState (0x60 bytes): delta time, sampling number, acceleration, angular velocity, angle,
// direction (3x3), attributes
void motion_state(uint8_t *s, int64_t sampling) {
    Motion m;
    if (g_lock) SDL_LockMutex(g_lock);
    m = g_now;
    if (g_lock) SDL_UnlockMutex(g_lock);
    memset(s, 0, 0x60);
    int64_t dt = 5000000;  // (5 ms, the hid's rate)
    memcpy(s, &dt, 8);
    memcpy(s + 0x08, &sampling, 8);
    memcpy(s + 0x10, m.accel, 12);
    memcpy(s + 0x1C, m.gyro, 12);
    memcpy(s + 0x28, m.angle, 12);
    memcpy(s + 0x34, m.dir, 36);
    uint32_t attr = 1;  // IsConnected
    memcpy(s + 0x58, &attr, 4);
}
bool motion_at_rest(void) {
    if (!g_lock) return true;
    SDL_LockMutex(g_lock);
    bool r = g_now.rest;
    SDL_UnlockMutex(g_lock);
    return r;
}
