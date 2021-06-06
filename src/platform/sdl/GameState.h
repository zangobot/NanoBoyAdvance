//
// Created by Luca Demetrio on 06/06/21.
//

#ifndef NANOBOYADVANCE_GAMESTATE_H
#define NANOBOYADVANCE_GAMESTATE_H

#include <atomic>
#include <common/log.hpp>
#include <cstdlib>
#include <cstring>
#include <emulator/config/config_toml.hpp>
#include <emulator/device/input_device.hpp>
#include <emulator/device/video_device.hpp>
#include <emulator/emulator.hpp>
#include <experimental/filesystem>
#include <fmt/format.h>
#include <iterator>
#include <optional>
#include <string>
#include <toml.hpp>
#include <unordered_map>

#include "../../external/pybind11/include/pybind11/pybind11.h"
#include "device/audio_device.hpp"

#include <GL/glew.h>
#include <string>

using namespace std;

struct KeyMap {
    SDL_Keycode fastforward = SDLK_SPACE;
    SDL_Keycode reset = SDLK_F9;
    SDL_Keycode fullscreen = SDLK_F10;
    std::unordered_map<SDL_Keycode, nba::InputDevice::Key> gba;
} keymap;

class GameState {
public:

    GameState();
    ~GameState();
    void update_controller();
    void update_fastforward(bool fastforward);
    void update_viewport();
    void loop();
    void update_key(SDL_KeyboardEvent* event);
    void init(string bios_file_path, string rom_file_path);

    void load_keymap();

    int kNativeWidth = 240;
    int kNativeHeight = 160;
    SDL_Window *g_window;
    SDL_GLContext g_gl_context;
    GLuint g_gl_texture;
    std::uint32_t g_framebuffer[38400];
    int g_frame_counter = 0;
    int g_swap_interval = 1;
    bool g_sync_to_audio = true;
    int g_cycles_per_audio_frame = 0;
    nba::BasicInputDevice g_keyboard_input_device = nba::BasicInputDevice{};
    nba::BasicInputDevice g_controller_input_device = nba::BasicInputDevice{};
    SDL_GameController *g_game_controller = nullptr;
    bool g_game_controller_button_x_old = false;
    bool g_fastforward = false;
    std::shared_ptr <nba::Config> g_config = std::make_shared<nba::Config>();
    std::unique_ptr <nba::Emulator> g_emulator = std::make_unique<nba::Emulator>(g_config);
    std::mutex g_emulator_lock;

};

struct CombinedInputDevice : public nba::InputDevice {
public:
    GameState* gs;
    CombinedInputDevice(GameState* gs);
    auto Poll(Key key) -> bool final;
    void SetOnChangeCallback(std::function<void(void)> callback);
};

struct SDL2_VideoDevice : public nba::VideoDevice {
public:
    GameState* gs;
    SDL2_VideoDevice(GameState* gs);
    void Draw(std::uint32_t* buffer) final;
};

#endif //NANOBOYADVANCE_GAMESTATE_H
