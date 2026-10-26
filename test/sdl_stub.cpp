// headless stand-ins for the SDL2 calls gw3nx makes (ARM64 backend test only)
#include <SDL2/SDL.h>
#include <chrono>
#include <thread>

extern "C" {
int SDL_Init(Uint32) { return 0; }
int SDL_InitSubSystem(Uint32) { return 0; }
const char* SDL_GetError(void) { return "stub"; }
int SDL_GL_SetAttribute(SDL_GLattr, int) { return 0; }
SDL_Window* SDL_CreateWindow(const char*, int, int, int, int, Uint32) { return (SDL_Window*)1; }
SDL_GLContext SDL_GL_CreateContext(SDL_Window*) { return (SDL_GLContext)1; }
void SDL_GL_SwapWindow(SDL_Window*) {}
int SDL_PollEvent(SDL_Event*) { return 0; }
Uint32 SDL_GetTicks(void) {
    static auto t0 = std::chrono::steady_clock::now();
    return (Uint32)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}
void SDL_Delay(Uint32 ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
int SDL_NumJoysticks(void) { return 0; }
SDL_bool SDL_IsGameController(int) { return SDL_FALSE; }
SDL_GameController* SDL_GameControllerOpen(int) { return nullptr; }
Uint8 SDL_GameControllerGetButton(SDL_GameController*, SDL_GameControllerButton) { return 0; }
Sint16 SDL_GameControllerGetAxis(SDL_GameController*, SDL_GameControllerAxis) { return 0; }
SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec* w, SDL_AudioSpec* h, int) { *h = *w; return 1; }
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID) { return 0; }
int SDL_QueueAudio(SDL_AudioDeviceID, const void*, Uint32) { return 0; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID, int) {}
void SDL_GL_GetDrawableSize(SDL_Window*, int* w, int* h) { *w = 1280; *h = 720; }
SDL_bool SDL_GameControllerGetAttached(SDL_GameController*) { return SDL_FALSE; }
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int) { return -1; }
void SDL_GameControllerClose(SDL_GameController*) {}
SDL_Joystick* SDL_GameControllerGetJoystick(SDL_GameController*) { return nullptr; }
const char* SDL_GameControllerName(SDL_GameController*) { return "stub"; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick*) { return -1; }
int SDL_GameControllerRumble(SDL_GameController*, Uint16, Uint16, Uint32) { return 0; }
void SDL_SetWindowSize(SDL_Window*, int, int) {}
}
