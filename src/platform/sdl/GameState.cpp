//
// Created by Luca Demetrio on 06/06/21.
//

#include "GameState.h"

auto load_as_string(std::string const& path) -> std::optional<std::string> {
    std::string ret;
    std::ifstream file { path };
    if (!file.good()) {
        return {};
    }
    file.seekg(0, std::ios::end);
    ret.reserve(file.tellg());
    file.seekg(0);
    ret.assign(std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{});
    return ret;
}

auto compile_shader(GLuint shader, const char* source) -> bool {
    GLint compiled = 0;
    const char* source_array[] = { source };
    glShaderSource(shader, 1, source_array, nullptr);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if(compiled == GL_FALSE) {
        GLint max_length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &max_length);

        auto error_log = std::make_unique<GLchar[]>(max_length);
        glGetShaderInfoLog(shader, max_length, &max_length, error_log.get());
        LOG_ERROR("Failed to compile shader:\n{0}", error_log.get());
        glDeleteShader(shader);
        return false;
    }

    return true;
}

GameState::GameState() {
    kNativeWidth = 240;
    kNativeHeight = 160;
    g_frame_counter = 0;
    g_swap_interval = 1;
    g_sync_to_audio = true;
    g_cycles_per_audio_frame = 0;
    g_keyboard_input_device = nba::BasicInputDevice{};
    g_controller_input_device = nba::BasicInputDevice{};
    g_game_controller = nullptr;
    g_game_controller_button_x_old = false;
    g_fastforward = false;
    g_config = std::make_shared<nba::Config>();
    g_emulator = std::make_unique<nba::Emulator>(g_config);
    //g_emulator_lock = std::mutex{};
}

void GameState::GameState::load_keymap() {
    toml::value data;

    try {
        data = toml::parse("keymap.toml");
    } catch (std::exception& ex) {
        LOG_WARN("Failed to load or parse keymap configuration.");
        return;
    }

    if (data.contains("general")) {
        auto general_result = toml::expect<toml::value>(data.at("general"));

        if (general_result.is_ok()) {
            auto general = general_result.unwrap();
            keymap.fastforward = SDL_GetKeyFromName(toml::find_or<std::string>(general, "fastforward", "Space").c_str());
            keymap.reset = SDL_GetKeyFromName(toml::find_or<std::string>(general, "reset", "F9").c_str());
            keymap.fullscreen = SDL_GetKeyFromName(toml::find_or<std::string>(general, "fullscreen", "F10").c_str());
        }
    }

    if (data.contains("gba")) {
        auto gba_result = toml::expect<toml::value>(data.at("gba"));

        if (gba_result.is_ok()) {
            auto gba = gba_result.unwrap();
            keymap.gba[SDL_GetKeyFromName(toml::find_or<std::string>(gba, "a", "A").c_str())] = nba::InputDevice::Key::A;
            keymap.gba[SDL_GetKeyFromName(toml::find_or<std::string>(gba, "b", "B").c_str())] = nba::InputDevice::Key::B;
            keymap.gba[SDL_GetKeyFromName(toml::find_or<std::string>(gba, "l", "D").c_str())] = nba::InputDevice::Key::L;
            keymap.gba[SDL_GetKeyFromName(toml::find_or<std::string>(gba, "r", "F").c_str())] = nba::InputDevice::Key::R;
            keymap.gba[SDL_GetKeyFromName(toml::find_or<std::string>(gba, "start", "Return").c_str())] = nba::InputDevice::Key::Start;
            keymap.gba[SDL_GetKeyFromName(toml::find_or<std::string>(gba, "select", "Backspace").c_str())] = nba::InputDevice::Key::Select;
            keymap.gba[SDL_GetKeyFromName(toml::find_or<std::string>(gba, "up", "Up").c_str())] = nba::InputDevice::Key::Up;
            keymap.gba[SDL_GetKeyFromName(toml::find_or<std::string>(gba, "down", "Down").c_str())] = nba::InputDevice::Key::Down;
            keymap.gba[SDL_GetKeyFromName(toml::find_or<std::string>(gba, "left", "Left").c_str())] = nba::InputDevice::Key::Left;
            keymap.gba[SDL_GetKeyFromName(toml::find_or<std::string>(gba, "right", "Right").c_str())] = nba::InputDevice::Key::Right;
        }
    }
}

void GameState::GameState::init(string bios_file_path, string rom_file_path) {
    namespace fs = std::experimental::filesystem;
    if (argc >= 1) {
        fs::current_path(fs::absolute(argv[0]).replace_filename(fs::path{ }));
    }
    common::logger::init();
    config_toml_read(*g_config, "config.toml");

    load_keymap();
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);
    g_window = SDL_CreateWindow("NanoBoyAdvance",
                                SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED,
                                kNativeWidth * g_config->video.scale,
                                kNativeHeight * g_config->video.scale,
                                SDL_WINDOW_OPENGL);
    g_gl_context = SDL_GL_CreateContext(g_window);
    glewInit();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_DisplayMode mode;
    SDL_GetCurrentDisplayMode(0, &mode);
    if (mode.refresh_rate % 60 == 0) {
        g_swap_interval = mode.refresh_rate / 60;
    }
    SDL_GL_SetSwapInterval(g_swap_interval);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &g_gl_texture);
    glBindTexture(GL_TEXTURE_2D, g_gl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (!g_config->video.shader.path_vs.empty() && !g_config->video.shader.path_fs.empty()) {
        auto vert_src = load_as_string(g_config->video.shader.path_vs);
        auto frag_src = load_as_string(g_config->video.shader.path_fs);
        if (vert_src.has_value() && frag_src.has_value()) {
            auto vid = glCreateShader(GL_VERTEX_SHADER);
            auto fid = glCreateShader(GL_FRAGMENT_SHADER);
            if (compile_shader(vid, vert_src.value().c_str()) && compile_shader(fid, frag_src.value().c_str())) {
                auto pid = glCreateProgram();
                glAttachShader(pid, vid);
                glAttachShader(pid, fid);
                glLinkProgram(pid);
                glUseProgram(pid);
            }
        }
    }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    g_sync_to_audio = g_config->sync_to_audio;

    update_viewport();
//    for (int i = 0; i < SDL_NumJoysticks(); i++) {
//        if (SDL_IsGameController(i)) {
//            g_game_controller = SDL_GameControllerOpen(i);
//            if (g_game_controller != nullptr) {
//                LOG_INFO("Detected game controller '{0}'", SDL_GameControllerNameForIndex(i));
//                break;
//            }
//        }
//    }
    auto audio_device = std::make_shared<SDL2_AudioDevice>();

    g_config->audio_dev = audio_device;
    g_config->input_dev = std::make_shared<CombinedInputDevice>();
    g_config->video_dev = std::make_shared<SDL2_VideoDevice>();
    g_emulator->Reset();
    g_cycles_per_audio_frame = 16777216ULL * audio_device->GetBlockSize() / audio_device->GetSampleRate();
}

void GameState::GameState::update_key(SDL_KeyboardEvent *event) {
    bool pressed = event->type == SDL_KEYDOWN;
    auto key = event->keysym.sym;

//    if (key == keymap.fastforward) {
//        update_fastforward(pressed);
//    }
//
//    if (key == keymap.reset && !pressed) {
//        g_emulator_lock.lock();
//        g_emulator->Reset();
//        g_emulator_lock.unlock();
//    }

    auto match = keymap.gba.find(key);
    if (match != keymap.gba.end()) {
        g_keyboard_input_device.SetKeyStatus(match->second, pressed);
    }
}

void GameState::GameState::loop() {
    auto event = SDL_Event{};

    auto ticks_start = SDL_GetTicks();

    for (;;) {
        update_controller();
        if (!g_sync_to_audio) {
            g_emulator_lock.lock();
            g_emulator->Frame();
            g_emulator_lock.unlock();
        }
        update_viewport();
        glClear(GL_COLOR_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, g_gl_texture);
        glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA,
                kNativeWidth,
                kNativeHeight,
                0,
                GL_BGRA,
                GL_UNSIGNED_BYTE,
                g_framebuffer //HERE IT IS THE SCREEN
        );
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0);
        glVertex2f(-1.0f, 1.0f);
        glTexCoord2f(1.0f, 0);
        glVertex2f(1.0f, 1.0f);
        glTexCoord2f(1.0f, 1.0f);
        glVertex2f(1.0f, -1.0f);
        glTexCoord2f(0, 1.0f);
        glVertex2f(-1.0f, -1.0f);
        glEnd();
        SDL_GL_SwapWindow(g_window);
        auto ticks_end = SDL_GetTicks();
        if ((ticks_end - ticks_start) >= 1000) {
            auto title = fmt::format("NanoBoyAdvance [{0} fps | {1}%]", g_frame_counter, int(g_frame_counter / 60.0 * 100.0));
            SDL_SetWindowTitle(g_window, title.c_str());
            g_frame_counter = 0;
            ticks_start = ticks_end;
        }
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                return;
            if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
                update_key(reinterpret_cast<SDL_KeyboardEvent*>(&event));
        }
    }
}

GameState::~GameState() {
    g_emulator_lock.lock();
    if (g_game_controller != nullptr) {
        SDL_GameControllerClose(g_game_controller);
    }
    SDL_GL_DeleteContext(g_gl_context);
    SDL_DestroyWindow(g_window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);
}

void GameState::GameState::update_viewport() {
    int width;
    int height;
    SDL_GL_GetDrawableSize(g_window, &width, &height);

    int viewport_width = height + height / 2;
    glViewport((width - viewport_width) / 2, 0, viewport_width, height);
}

void GameState::GameState::update_fastforward(bool fastforward) {
    g_fastforward = fastforward;
    g_sync_to_audio = !fastforward && g_config->sync_to_audio;
    if (fastforward) {
        SDL_GL_SetSwapInterval(0);
    } else {
        SDL_GL_SetSwapInterval(g_swap_interval);
    }
}

void GameState::GameState::update_controller() {
    if (g_game_controller == nullptr)
        return;
    SDL_GameControllerUpdate();

    auto button_x = SDL_GameControllerGetButton(g_game_controller, SDL_CONTROLLER_BUTTON_X);
    if (g_game_controller_button_x_old && !button_x) {
        update_fastforward(!g_fastforward);
    }
    g_game_controller_button_x_old = button_x;

    static const std::unordered_map<SDL_GameControllerButton, nba::InputDevice::Key> buttons{
            {SDL_CONTROLLER_BUTTON_A,             nba::InputDevice::Key::A},
            {SDL_CONTROLLER_BUTTON_B,             nba::InputDevice::Key::B},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  nba::InputDevice::Key::L},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, nba::InputDevice::Key::R},
            {SDL_CONTROLLER_BUTTON_START,         nba::InputDevice::Key::Start},
            {SDL_CONTROLLER_BUTTON_BACK,          nba::InputDevice::Key::Select}
    };

    for (auto &button : buttons) {
        if (SDL_GameControllerGetButton(g_game_controller, button.first)) {
            g_controller_input_device.SetKeyStatus(button.second, true);
        } else {
            g_controller_input_device.SetKeyStatus(button.second, false);
        }
    }

    constexpr auto threshold = std::numeric_limits<int16_t>::max() / 2;
    auto x = SDL_GameControllerGetAxis(g_game_controller, SDL_CONTROLLER_AXIS_LEFTX);
    auto y = SDL_GameControllerGetAxis(g_game_controller, SDL_CONTROLLER_AXIS_LEFTY);

    g_controller_input_device.SetKeyStatus(nba::InputDevice::Key::Left, x < -threshold);
    g_controller_input_device.SetKeyStatus(nba::InputDevice::Key::Right, x > threshold);
    g_controller_input_device.SetKeyStatus(nba::InputDevice::Key::Up, y < -threshold);
    g_controller_input_device.SetKeyStatus(nba::InputDevice::Key::Down, y > threshold);
}

CombinedInputDevice::CombinedInputDevice(GameState *gs) {
    this->gs = gs;
}

auto CombinedInputDevice::Poll(Key key) -> bool {
    return this->gs->g_keyboard_input_device.Poll(key) ||
           this->gs->g_controller_input_device.Poll(key);
}

void CombinedInputDevice::SetOnChangeCallback(std::function<void()> callback) {
    this->gs->g_keyboard_input_device.SetOnChangeCallback(callback);
    this->gs->g_controller_input_device.SetOnChangeCallback(callback);
}

void SDL2_VideoDevice::Draw(std::uint32_t *buffer) {
    std::memcpy(this->gs.g_framebuffer, buffer,
                sizeof(std::uint32_t) * this->gs.kNativeWidth * this->gs.kNativeHeight);
    this->gs->g_frame_counter++;
}
