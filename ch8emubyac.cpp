#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kChip8Width = 64;
constexpr int kChip8Height = 32;
constexpr int kWindowWidth = 600;
constexpr int kWindowHeight = 400;
constexpr int kCpuHz = 700;

struct Button {
    SDL_Rect rect {};
    std::string label;
    bool active = true;
};

enum class MenuAction {
    None,
    OpenRomPicker,
    OpenRomPath,
    LoadCurrent,
    ToggleRun,
    Step,
    Reset,
    Speed1x,
    Speed2x,
    Speed4x,
    Quit
};

struct MenuEntry {
    std::string label;
    MenuAction action = MenuAction::None;
};

struct MenuTab {
    SDL_Rect rect {};
    std::string label;
    std::vector<MenuEntry> entries;
};

class Chip8 {
public:
    Chip8() {
        reset();
    }

    void reset() {
        memory.fill(0);
        V.fill(0);
        stack.fill(0);
        gfx.fill(0);
        keypad.fill(0);
        I = 0;
        pc = 0x200;
        sp = 0;
        delayTimer = 0;
        soundTimer = 0;
        waitingForKey = false;
        waitingRegister = 0;

        for (size_t i = 0; i < fontSet.size(); ++i) {
            memory[0x50 + i] = fontSet[i];
        }
    }

    bool loadRom(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            std::cerr << "Could not open ROM: " << path << "\n";
            return false;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (size <= 0 || size > 3584) {
            std::cerr << "Invalid ROM size: " << size << "\n";
            return false;
        }

        std::vector<char> buffer(static_cast<size_t>(size));
        if (!file.read(buffer.data(), size)) {
            std::cerr << "Failed to read ROM\n";
            return false;
        }

        reset();
        for (size_t i = 0; i < buffer.size(); ++i) {
            memory[0x200 + i] = static_cast<uint8_t>(buffer[i]);
        }
        return true;
    }

    void tickTimers() {
        if (delayTimer > 0) {
            --delayTimer;
        }
        if (soundTimer > 0) {
            --soundTimer;
        }
    }

    void setKey(uint8_t key, bool pressed) {
        if (key > 0xF) return;
        keypad[key] = pressed ? 1 : 0;

        if (waitingForKey && pressed) {
            V[waitingRegister] = key;
            waitingForKey = false;
            pc += 2;
        }
    }

    void emulateCycle() {
        if (waitingForKey) return;
        uint16_t opcode = static_cast<uint16_t>(memory[pc] << 8U) | memory[pc + 1];

        switch (opcode & 0xF000) {
            case 0x0000:
                switch (opcode & 0x00FF) {
                    case 0x00E0:
                        gfx.fill(0);
                        pc += 2;
                        break;
                    case 0x00EE:
                        --sp;
                        pc = stack[sp];
                        pc += 2;
                        break;
                    default:
                        pc += 2;
                        break;
                }
                break;
            case 0x1000:
                pc = opcode & 0x0FFF;
                break;
            case 0x2000:
                stack[sp] = pc;
                ++sp;
                pc = opcode & 0x0FFF;
                break;
            case 0x3000:
                pc += (V[(opcode & 0x0F00) >> 8] == (opcode & 0x00FF)) ? 4 : 2;
                break;
            case 0x4000:
                pc += (V[(opcode & 0x0F00) >> 8] != (opcode & 0x00FF)) ? 4 : 2;
                break;
            case 0x5000:
                pc += (V[(opcode & 0x0F00) >> 8] == V[(opcode & 0x00F0) >> 4]) ? 4 : 2;
                break;
            case 0x6000:
                V[(opcode & 0x0F00) >> 8] = opcode & 0x00FF;
                pc += 2;
                break;
            case 0x7000:
                V[(opcode & 0x0F00) >> 8] += opcode & 0x00FF;
                pc += 2;
                break;
            case 0x8000: {
                uint8_t x = (opcode & 0x0F00) >> 8;
                uint8_t y = (opcode & 0x00F0) >> 4;
                switch (opcode & 0x000F) {
                    case 0x0:
                        V[x] = V[y];
                        break;
                    case 0x1:
                        V[x] |= V[y];
                        break;
                    case 0x2:
                        V[x] &= V[y];
                        break;
                    case 0x3:
                        V[x] ^= V[y];
                        break;
                    case 0x4: {
                        uint16_t sum = V[x] + V[y];
                        V[0xF] = sum > 0xFF;
                        V[x] = static_cast<uint8_t>(sum & 0xFF);
                        break;
                    }
                    case 0x5:
                        V[0xF] = V[x] > V[y];
                        V[x] -= V[y];
                        break;
                    case 0x6:
                        V[0xF] = V[x] & 0x01;
                        V[x] >>= 1;
                        break;
                    case 0x7:
                        V[0xF] = V[y] > V[x];
                        V[x] = V[y] - V[x];
                        break;
                    case 0xE:
                        V[0xF] = (V[x] >> 7) & 0x01;
                        V[x] <<= 1;
                        break;
                    default:
                        break;
                }
                pc += 2;
                break;
            }
            case 0x9000:
                pc += (V[(opcode & 0x0F00) >> 8] != V[(opcode & 0x00F0) >> 4]) ? 4 : 2;
                break;
            case 0xA000:
                I = opcode & 0x0FFF;
                pc += 2;
                break;
            case 0xB000:
                pc = (opcode & 0x0FFF) + V[0];
                break;
            case 0xC000: {
                static std::mt19937 rng(std::random_device{}());
                std::uniform_int_distribution<int> dist(0, 255);
                uint8_t rnd = static_cast<uint8_t>(dist(rng));
                V[(opcode & 0x0F00) >> 8] = rnd & (opcode & 0x00FF);
                pc += 2;
                break;
            }
            case 0xD000: {
                uint8_t x = V[(opcode & 0x0F00) >> 8] % kChip8Width;
                uint8_t y = V[(opcode & 0x00F0) >> 4] % kChip8Height;
                uint8_t h = opcode & 0x000F;
                V[0xF] = 0;

                for (int row = 0; row < h; ++row) {
                    uint8_t sprite = memory[I + row];
                    for (int col = 0; col < 8; ++col) {
                        if ((sprite & (0x80 >> col)) == 0) continue;
                        int px = (x + col) % kChip8Width;
                        int py = (y + row) % kChip8Height;
                        int idx = py * kChip8Width + px;
                        if (gfx[idx] == 1) V[0xF] = 1;
                        gfx[idx] ^= 1;
                    }
                }
                pc += 2;
                break;
            }
            case 0xE000: {
                uint8_t x = (opcode & 0x0F00) >> 8;
                switch (opcode & 0x00FF) {
                    case 0x9E:
                        pc += keypad[V[x] & 0x0F] ? 4 : 2;
                        break;
                    case 0xA1:
                        pc += keypad[V[x] & 0x0F] ? 2 : 4;
                        break;
                    default:
                        pc += 2;
                        break;
                }
                break;
            }
            case 0xF000: {
                uint8_t x = (opcode & 0x0F00) >> 8;
                switch (opcode & 0x00FF) {
                    case 0x07:
                        V[x] = delayTimer;
                        pc += 2;
                        break;
                    case 0x0A:
                        waitingForKey = true;
                        waitingRegister = x;
                        break;
                    case 0x15:
                        delayTimer = V[x];
                        pc += 2;
                        break;
                    case 0x18:
                        soundTimer = V[x];
                        pc += 2;
                        break;
                    case 0x1E:
                        I += V[x];
                        pc += 2;
                        break;
                    case 0x29:
                        I = 0x50 + (V[x] * 5);
                        pc += 2;
                        break;
                    case 0x33:
                        memory[I] = V[x] / 100;
                        memory[I + 1] = (V[x] / 10) % 10;
                        memory[I + 2] = V[x] % 10;
                        pc += 2;
                        break;
                    case 0x55:
                        for (int i = 0; i <= x; ++i) memory[I + i] = V[i];
                        pc += 2;
                        break;
                    case 0x65:
                        for (int i = 0; i <= x; ++i) V[i] = memory[I + i];
                        pc += 2;
                        break;
                    default:
                        pc += 2;
                        break;
                }
                break;
            }
            default:
                pc += 2;
                break;
        }
    }

    const std::array<uint8_t, kChip8Width * kChip8Height>& display() const {
        return gfx;
    }

private:
    std::array<uint8_t, 4096> memory {};
    std::array<uint8_t, 16> V {};
    uint16_t I = 0;
    uint16_t pc = 0x200;
    std::array<uint16_t, 16> stack {};
    uint8_t sp = 0;
    uint8_t delayTimer = 0;
    uint8_t soundTimer = 0;
    std::array<uint8_t, 16> keypad {};
    std::array<uint8_t, kChip8Width * kChip8Height> gfx {};
    bool waitingForKey = false;
    uint8_t waitingRegister = 0;

    static constexpr std::array<uint8_t, 80> fontSet = {
        0xF0, 0x90, 0x90, 0x90, 0xF0, 0x20, 0x60, 0x20, 0x20, 0x70, 0xF0, 0x10, 0xF0, 0x80, 0xF0, 0xF0,
        0x10, 0xF0, 0x10, 0xF0, 0x90, 0x90, 0xF0, 0x10, 0x10, 0xF0, 0x80, 0xF0, 0x10, 0xF0, 0xF0, 0x80,
        0xF0, 0x90, 0xF0, 0xF0, 0x10, 0x20, 0x40, 0x40, 0xF0, 0x90, 0xF0, 0x90, 0xF0, 0xF0, 0x90, 0xF0,
        0x10, 0xF0, 0xF0, 0x90, 0xF0, 0x90, 0x90, 0xE0, 0x90, 0xE0, 0x90, 0xE0, 0xF0, 0x80, 0x80, 0x80,
        0xF0, 0xE0, 0x90, 0x90, 0x90, 0xE0, 0xF0, 0x80, 0xF0, 0x80, 0xF0, 0xF0, 0x80, 0xF0, 0x80, 0x80
    };
};

uint8_t keyFromScancode(SDL_Scancode code) {
    switch (code) {
        case SDL_SCANCODE_X: return 0x0;
        case SDL_SCANCODE_1: return 0x1;
        case SDL_SCANCODE_2: return 0x2;
        case SDL_SCANCODE_3: return 0x3;
        case SDL_SCANCODE_Q: return 0x4;
        case SDL_SCANCODE_W: return 0x5;
        case SDL_SCANCODE_E: return 0x6;
        case SDL_SCANCODE_A: return 0x7;
        case SDL_SCANCODE_S: return 0x8;
        case SDL_SCANCODE_D: return 0x9;
        case SDL_SCANCODE_Z: return 0xA;
        case SDL_SCANCODE_C: return 0xB;
        case SDL_SCANCODE_4: return 0xC;
        case SDL_SCANCODE_R: return 0xD;
        case SDL_SCANCODE_F: return 0xE;
        case SDL_SCANCODE_V: return 0xF;
        default: return 0xFF;
    }
}

bool inside(const SDL_Rect& r, int x, int y) {
    return x >= r.x && y >= r.y && x < (r.x + r.w) && y < (r.y + r.h);
}

void drawButton(SDL_Renderer* renderer, TTF_Font* font, const Button& b, SDL_Color textColor) {
    SDL_SetRenderDrawColor(renderer, b.active ? 30 : 19, b.active ? 30 : 19, b.active ? 34 : 19, 255);
    SDL_RenderFillRect(renderer, &b.rect);
    SDL_SetRenderDrawColor(renderer, b.active ? 74 : 50, b.active ? 110 : 50, b.active ? 173 : 50, 255);
    SDL_RenderDrawRect(renderer, &b.rect);

    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, b.label.c_str(), textColor);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (!tex) {
        SDL_FreeSurface(surf);
        return;
    }
    SDL_Rect dst {
        b.rect.x + (b.rect.w - surf->w) / 2,
        b.rect.y + (b.rect.h - surf->h) / 2,
        surf->w,
        surf->h
    };
    SDL_RenderCopy(renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

void drawText(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, int x, int y, SDL_Color color) {
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (!tex) {
        SDL_FreeSurface(surf);
        return;
    }
    SDL_Rect dst {x, y, surf->w, surf->h};
    SDL_RenderCopy(renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

void drawMenuDropdown(SDL_Renderer* renderer, TTF_Font* font, int x, int y, int w, const std::vector<MenuEntry>& entries, int hovered) {
    SDL_Rect panel {x, y, w, static_cast<int>(entries.size()) * 24 + 6};
    SDL_SetRenderDrawColor(renderer, 24, 24, 30, 255);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 70, 78, 98, 255);
    SDL_RenderDrawRect(renderer, &panel);

    for (size_t i = 0; i < entries.size(); ++i) {
        SDL_Rect row {x + 3, y + 3 + static_cast<int>(i) * 24, w - 6, 22};
        if (static_cast<int>(i) == hovered) {
            SDL_SetRenderDrawColor(renderer, 48, 62, 90, 255);
            SDL_RenderFillRect(renderer, &row);
        }
        drawText(renderer, font, entries[i].label, row.x + 8, row.y + 4, SDL_Color{168, 184, 220, 255});
    }
}

bool loadRomTracked(Chip8& emu, std::string& romPath, std::string candidate, bool& running, std::string& statusText) {
    if (emu.loadRom(candidate)) {
        romPath = std::move(candidate);
        running = true;
        statusText = "ROM booted: " + romPath;
        return true;
    }
    running = false;
    statusText = "Load failed: " + candidate;
    return false;
}

std::string trimNewlines(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

std::string openRomPathWithNativeDialog() {
#if defined(__APPLE__)
    const char* cmd =
        "osascript -e 'set f to choose file with prompt \"Select a CHIP-8 ROM\" of type {\"ch8\"}' "
        "-e 'POSIX path of f'";
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return {};
    char buffer[4096];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    int rc = pclose(pipe);
    if (rc != 0) return {};
    return trimNewlines(output);
#else
    return {};
#endif
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << "\n";
        return 1;
    }
    if (TTF_Init() != 0) {
        std::cerr << "TTF init failed: " << TTF_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    SDL_Window* window = SDL_CreateWindow(
        "Chip8emulator 0.1 by ac",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        kWindowWidth,
        kWindowHeight,
        SDL_WINDOW_SHOWN
    );
    if (!window) {
        std::cerr << "Window creation failed: " << SDL_GetError() << "\n";
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::cerr << "Renderer creation failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    TTF_Font* font = TTF_OpenFont("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 13);
    if (!font) {
        std::cerr << "Font load failed: " << TTF_GetError() << "\n";
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    Chip8 emu;
    std::string romPath;
    if (argc > 1) {
        romPath = argv[1];
    }
    bool loaded = !romPath.empty() && emu.loadRom(romPath);
    bool running = loaded;
    std::string statusText = loaded ? "ROM booted: " + romPath : "No ROM loaded. Drop a .ch8 file into window.";
    int cpuMultiplier = 1;

    std::vector<Button> buttons = {
        {{362, 44, 216, 32}, "Load Current ROM", true},
        {{362, 82, 216, 32}, "Run / Pause", true},
        {{362, 120, 216, 32}, "Step", true},
        {{362, 158, 216, 32}, "Reset", true}
    };

    bool quit = false;
    auto lastTimerTick = std::chrono::high_resolution_clock::now();
    auto lastCpuTick = std::chrono::high_resolution_clock::now();
    auto cpuInterval = std::chrono::microseconds(1000000 / kCpuHz);

    std::vector<MenuTab> menuTabs = {
        {{8, 3, 52, 22}, "File", {{"Open ROM...", MenuAction::OpenRomPicker}, {"Open ROM Path...", MenuAction::OpenRomPath}, {"Load Current ROM", MenuAction::LoadCurrent}, {"Reset ROM", MenuAction::Reset}, {"Quit", MenuAction::Quit}}},
        {{66, 3, 88, 22}, "Emulation", {{"Run / Pause", MenuAction::ToggleRun}, {"Step Cycle", MenuAction::Step}}},
        {{160, 3, 62, 22}, "Tools", {{"Speed x1", MenuAction::Speed1x}, {"Speed x2", MenuAction::Speed2x}, {"Speed x4", MenuAction::Speed4x}}},
        {{228, 3, 56, 22}, "Help", {{"Drop .ch8 file to load", MenuAction::None}}}
    };
    int activeMenu = -1;
    int hoveredMenuEntry = -1;
    bool romPathInputMode = false;
    std::string romPathInputBuffer;
    SDL_Rect romPromptBox {74, 102, 452, 188};
    SDL_Rect romPromptTitleBar {74, 102, 452, 28};
    SDL_Rect romPromptInput {94, 170, 332, 30};
    SDL_Rect romPromptBrowseBtn {434, 170, 72, 30};
    SDL_Rect romPromptLoadBtn {324, 236, 88, 30};
    SDL_Rect romPromptCancelBtn {418, 236, 88, 30};

    auto runMenuAction = [&](MenuAction action) {
        switch (action) {
            case MenuAction::OpenRomPicker: {
                const std::string pickedPath = openRomPathWithNativeDialog();
                if (pickedPath.empty()) {
                    statusText = "Open ROM canceled";
                } else {
                    loaded = loadRomTracked(emu, romPath, pickedPath, running, statusText);
                }
                break;
            }
            case MenuAction::OpenRomPath:
                romPathInputMode = true;
                romPathInputBuffer = romPath;
                SDL_StartTextInput();
                statusText = "Type ROM path, press Enter to boot, Esc to cancel";
                break;
            case MenuAction::LoadCurrent:
                if (romPath.empty()) {
                    statusText = "No ROM selected. Start with: ./chip8emu /path/to/game.ch8 or drag/drop file.";
                    loaded = false;
                    running = false;
                } else {
                    loaded = loadRomTracked(emu, romPath, romPath, running, statusText);
                }
                break;
            case MenuAction::ToggleRun:
                if (loaded) {
                    running = !running;
                    statusText = running ? "Running" : "Paused";
                } else {
                    statusText = "No ROM loaded";
                }
                break;
            case MenuAction::Step:
                if (loaded) {
                    emu.emulateCycle();
                    statusText = "Single-step cycle executed";
                } else {
                    statusText = "No ROM loaded";
                }
                break;
            case MenuAction::Reset:
                if (loaded) {
                    bool resetOk = loadRomTracked(emu, romPath, romPath, running, statusText);
                    loaded = resetOk;
                    if (resetOk) statusText = "ROM reset: " + romPath;
                } else {
                    statusText = "No ROM loaded";
                }
                break;
            case MenuAction::Speed1x:
                cpuMultiplier = 1;
                cpuInterval = std::chrono::microseconds(1000000 / (kCpuHz * cpuMultiplier));
                statusText = "CPU speed set to x1";
                break;
            case MenuAction::Speed2x:
                cpuMultiplier = 2;
                cpuInterval = std::chrono::microseconds(1000000 / (kCpuHz * cpuMultiplier));
                statusText = "CPU speed set to x2";
                break;
            case MenuAction::Speed4x:
                cpuMultiplier = 4;
                cpuInterval = std::chrono::microseconds(1000000 / (kCpuHz * cpuMultiplier));
                statusText = "CPU speed set to x4";
                break;
            case MenuAction::Quit:
                quit = true;
                break;
            case MenuAction::None:
                statusText = "Use menu items to control emulator";
                break;
        }
    };

    while (!quit) {
        SDL_Event e {};
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;

            if (romPathInputMode) {
                if (e.type == SDL_TEXTINPUT) {
                    romPathInputBuffer += e.text.text;
                    continue;
                }
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    const int mx = e.button.x;
                    const int my = e.button.y;
                    if (inside(romPromptBrowseBtn, mx, my)) {
                        const std::string picked = openRomPathWithNativeDialog();
                        if (!picked.empty()) {
                            romPathInputBuffer = picked;
                            statusText = "ROM selected in picker";
                        } else {
                            statusText = "Open ROM canceled";
                        }
                        continue;
                    }
                    if (inside(romPromptLoadBtn, mx, my)) {
                        romPathInputMode = false;
                        SDL_StopTextInput();
                        if (!romPathInputBuffer.empty()) {
                            loaded = loadRomTracked(emu, romPath, romPathInputBuffer, running, statusText);
                        } else {
                            statusText = "No ROM path entered";
                        }
                        continue;
                    }
                    if (inside(romPromptCancelBtn, mx, my)) {
                        romPathInputMode = false;
                        SDL_StopTextInput();
                        statusText = "ROM path input cancelled";
                        continue;
                    }
                }
                if (e.type == SDL_KEYDOWN) {
                    if (e.key.keysym.sym == SDLK_BACKSPACE && !romPathInputBuffer.empty()) {
                        romPathInputBuffer.pop_back();
                        continue;
                    }
                    if (e.key.keysym.sym == SDLK_ESCAPE) {
                        romPathInputMode = false;
                        SDL_StopTextInput();
                        statusText = "ROM path input cancelled";
                        continue;
                    }
                    if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                        romPathInputMode = false;
                        SDL_StopTextInput();
                        if (!romPathInputBuffer.empty()) {
                            loaded = loadRomTracked(emu, romPath, romPathInputBuffer, running, statusText);
                        } else {
                            statusText = "No ROM path entered";
                        }
                        continue;
                    }
                }
            }

            if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                uint8_t key = keyFromScancode(e.key.keysym.scancode);
                if (key != 0xFF) emu.setKey(key, e.type == SDL_KEYDOWN);
                const bool isCmdOrCtrl = (e.key.keysym.mod & KMOD_GUI) || (e.key.keysym.mod & KMOD_CTRL);
                if (e.type == SDL_KEYDOWN && isCmdOrCtrl && e.key.keysym.sym == SDLK_o) {
                    runMenuAction(MenuAction::OpenRomPicker);
                } else if (e.type == SDL_KEYDOWN && isCmdOrCtrl && e.key.keysym.sym == SDLK_q) {
                    quit = true;
                } else if (e.type == SDL_KEYDOWN && isCmdOrCtrl && e.key.keysym.sym == SDLK_r) {
                    runMenuAction(MenuAction::Reset);
                } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_SPACE && loaded) {
                    running = !running;
                    statusText = running ? "Running" : "Paused";
                } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_n && loaded) {
                    emu.emulateCycle();
                    statusText = "Single-step cycle executed";
                } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_r && loaded) {
                    loaded = emu.loadRom(romPath);
                    running = loaded;
                    statusText = loaded ? "ROM reset: " + romPath : "Reset failed";
                }
            }

            if (e.type == SDL_DROPFILE) {
                char* dropped = e.drop.file;
                if (dropped) {
                    romPath = dropped;
                    loaded = emu.loadRom(romPath);
                    running = loaded;
                    statusText = loaded ? "ROM booted: " + romPath : "ROM load failed: " + romPath;
                    SDL_free(dropped);
                }
            }

            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = e.button.y;
                bool consumed = false;

                for (size_t i = 0; i < menuTabs.size(); ++i) {
                    if (inside(menuTabs[i].rect, mx, my)) {
                        activeMenu = (activeMenu == static_cast<int>(i)) ? -1 : static_cast<int>(i);
                        hoveredMenuEntry = -1;
                        consumed = true;
                        break;
                    }
                }

                if (!consumed && activeMenu >= 0) {
                    const auto& tab = menuTabs[activeMenu];
                    SDL_Rect panel {tab.rect.x, 28, 180, static_cast<int>(tab.entries.size()) * 24 + 6};
                    if (inside(panel, mx, my)) {
                        int idx = (my - panel.y - 3) / 24;
                        if (idx >= 0 && idx < static_cast<int>(tab.entries.size())) {
                            runMenuAction(tab.entries[idx].action);
                        }
                        activeMenu = -1;
                        hoveredMenuEntry = -1;
                        consumed = true;
                    } else {
                        activeMenu = -1;
                        hoveredMenuEntry = -1;
                    }
                }

                if (consumed) continue;

                if (inside(buttons[0].rect, mx, my)) {
                    if (romPath.empty()) {
                        statusText = "No ROM selected. Start with: ./chip8emu /path/to/game.ch8 or drag/drop file.";
                        loaded = false;
                        running = false;
                    } else {
                        loaded = loadRomTracked(emu, romPath, romPath, running, statusText);
                    }
                } else if (inside(buttons[1].rect, mx, my)) {
                    running = !running;
                    statusText = running ? "Running" : "Paused";
                } else if (inside(buttons[2].rect, mx, my)) {
                    if (loaded) emu.emulateCycle();
                    statusText = loaded ? "Single-step cycle executed" : "No ROM loaded";
                } else if (inside(buttons[3].rect, mx, my)) {
                    if (loaded) {
                        emu.reset();
                        bool resetOk = loadRomTracked(emu, romPath, romPath, running, statusText);
                        loaded = resetOk;
                        if (resetOk) statusText = "ROM reset: " + romPath;
                    }
                }
            }

            if (e.type == SDL_MOUSEMOTION && activeMenu >= 0) {
                int mx = e.motion.x;
                int my = e.motion.y;
                const auto& tab = menuTabs[activeMenu];
                SDL_Rect panel {tab.rect.x, 28, 180, static_cast<int>(tab.entries.size()) * 24 + 6};
                if (inside(panel, mx, my)) {
                    int idx = (my - panel.y - 3) / 24;
                    hoveredMenuEntry = (idx >= 0 && idx < static_cast<int>(tab.entries.size())) ? idx : -1;
                } else {
                    hoveredMenuEntry = -1;
                }
            }
        }

        auto now = std::chrono::high_resolution_clock::now();
        while (running && now - lastCpuTick >= cpuInterval) {
            emu.emulateCycle();
            lastCpuTick += cpuInterval;
        }
        if (now - lastTimerTick >= std::chrono::milliseconds(16)) {
            emu.tickTimers();
            lastTimerTick = now;
        }

        SDL_SetRenderDrawColor(renderer, 8, 8, 10, 255);
        SDL_RenderClear(renderer);

        SDL_Rect menubar {0, 0, kWindowWidth, 28};
        SDL_SetRenderDrawColor(renderer, 26, 26, 30, 255);
        SDL_RenderFillRect(renderer, &menubar);
        SDL_SetRenderDrawColor(renderer, 58, 58, 68, 255);
        SDL_RenderDrawLine(renderer, 0, 28, kWindowWidth, 28);

        SDL_Rect sidebar {350, 28, 250, 338};
        SDL_SetRenderDrawColor(renderer, 18, 18, 22, 255);
        SDL_RenderFillRect(renderer, &sidebar);
        SDL_SetRenderDrawColor(renderer, 52, 52, 58, 255);
        SDL_RenderDrawLine(renderer, 350, 28, 350, 366);

        SDL_Rect statusbar {0, 366, kWindowWidth, 34};
        SDL_SetRenderDrawColor(renderer, 22, 22, 26, 255);
        SDL_RenderFillRect(renderer, &statusbar);
        SDL_SetRenderDrawColor(renderer, 58, 58, 68, 255);
        SDL_RenderDrawLine(renderer, 0, 366, kWindowWidth, 366);

        SDL_Rect displayArea {20, 44, 320, 160};
        SDL_SetRenderDrawColor(renderer, 70, 96, 136, 255);
        SDL_RenderDrawRect(renderer, &displayArea);
        SDL_Rect displayInner {displayArea.x + 1, displayArea.y + 1, displayArea.w - 2, displayArea.h - 2};
        SDL_SetRenderDrawColor(renderer, 6, 8, 14, 255);
        SDL_RenderFillRect(renderer, &displayInner);

        int pixelW = displayArea.w / kChip8Width;
        int pixelH = displayArea.h / kChip8Height;
        const auto& pixels = emu.display();
        for (int y = 0; y < kChip8Height; ++y) {
            for (int x = 0; x < kChip8Width; ++x) {
                if (!pixels[y * kChip8Width + x]) continue;
                SDL_Rect p {
                    displayArea.x + x * pixelW,
                    displayArea.y + y * pixelH,
                    pixelW,
                    pixelH
                };
                SDL_SetRenderDrawColor(renderer, 98, 162, 255, 255);
                SDL_RenderFillRect(renderer, &p);
            }
        }

        SDL_Color blue {120, 165, 245, 255};
        for (const auto& b : buttons) {
            drawButton(renderer, font, b, blue);
        }

        for (size_t i = 0; i < menuTabs.size(); ++i) {
            const auto& tab = menuTabs[i];
            if (activeMenu == static_cast<int>(i)) {
                SDL_SetRenderDrawColor(renderer, 42, 52, 72, 255);
                SDL_RenderFillRect(renderer, &tab.rect);
            }
            drawText(renderer, font, tab.label, tab.rect.x + 8, tab.rect.y + 4, SDL_Color{178, 182, 196, 255});
        }
        if (activeMenu >= 0) {
            const auto& tab = menuTabs[activeMenu];
            drawMenuDropdown(renderer, font, tab.rect.x, 28, 180, tab.entries, hoveredMenuEntry);
        }
        drawText(renderer, font, "CHIP-8 DISPLAY", 20, 24, SDL_Color{120, 151, 212, 255});
        drawText(renderer, font, "CONTROL PANEL", 368, 14, SDL_Color{122, 148, 190, 255});
        drawText(renderer, font, "Drop .ch8 file into window", 362, 206, SDL_Color{103, 121, 156, 255});
        drawText(renderer, font, "Cmd/Ctrl+O Open  Space Pause  N Step  Cmd/Ctrl+R Reset", 362, 228, SDL_Color{103, 121, 156, 255});
        drawText(renderer, font, "CHIP-8 KEYS", 362, 258, SDL_Color{112, 142, 190, 255});
        drawText(renderer, font, "1 2 3 4", 362, 278, SDL_Color{100, 124, 168, 255});
        drawText(renderer, font, "Q W E R", 362, 296, SDL_Color{100, 124, 168, 255});
        drawText(renderer, font, "A S D F", 442, 278, SDL_Color{100, 124, 168, 255});
        drawText(renderer, font, "Z X C V", 442, 296, SDL_Color{100, 124, 168, 255});

        if (romPathInputMode) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_Rect dimmer {0, 0, kWindowWidth, kWindowHeight};
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 120);
            SDL_RenderFillRect(renderer, &dimmer);

            SDL_SetRenderDrawColor(renderer, 236, 240, 246, 255);
            SDL_RenderFillRect(renderer, &romPromptBox);
            SDL_SetRenderDrawColor(renderer, 94, 110, 132, 255);
            SDL_RenderDrawRect(renderer, &romPromptBox);

            SDL_SetRenderDrawColor(renderer, 43, 108, 200, 255);
            SDL_RenderFillRect(renderer, &romPromptTitleBar);
            drawText(renderer, font, "Open ROM - Chip8emulator 0.1 by ac", romPromptTitleBar.x + 10, romPromptTitleBar.y + 6, SDL_Color{235, 243, 255, 255});

            drawText(renderer, font, "ROM file path (.ch8):", romPromptBox.x + 20, romPromptBox.y + 46, SDL_Color{58, 72, 92, 255});
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderFillRect(renderer, &romPromptInput);
            SDL_SetRenderDrawColor(renderer, 123, 139, 166, 255);
            SDL_RenderDrawRect(renderer, &romPromptInput);
            drawText(renderer, font, romPathInputBuffer, romPromptInput.x + 8, romPromptInput.y + 7, SDL_Color{42, 52, 66, 255});

            SDL_SetRenderDrawColor(renderer, 238, 243, 250, 255);
            SDL_RenderFillRect(renderer, &romPromptBrowseBtn);
            SDL_SetRenderDrawColor(renderer, 123, 139, 166, 255);
            SDL_RenderDrawRect(renderer, &romPromptBrowseBtn);
            drawText(renderer, font, "Browse...", romPromptBrowseBtn.x + 8, romPromptBrowseBtn.y + 7, SDL_Color{42, 56, 76, 255});

            SDL_SetRenderDrawColor(renderer, 43, 108, 200, 255);
            SDL_RenderFillRect(renderer, &romPromptLoadBtn);
            SDL_SetRenderDrawColor(renderer, 31, 91, 176, 255);
            SDL_RenderDrawRect(renderer, &romPromptLoadBtn);
            drawText(renderer, font, "Load", romPromptLoadBtn.x + 28, romPromptLoadBtn.y + 7, SDL_Color{236, 246, 255, 255});

            SDL_SetRenderDrawColor(renderer, 238, 243, 250, 255);
            SDL_RenderFillRect(renderer, &romPromptCancelBtn);
            SDL_SetRenderDrawColor(renderer, 123, 139, 166, 255);
            SDL_RenderDrawRect(renderer, &romPromptCancelBtn);
            drawText(renderer, font, "Cancel", romPromptCancelBtn.x + 20, romPromptCancelBtn.y + 7, SDL_Color{52, 66, 86, 255});

            drawText(renderer, font, "Enter = Load   Esc = Cancel", romPromptBox.x + 20, romPromptBox.y + 108, SDL_Color{90, 104, 124, 255});
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }

        std::ostringstream fpsLike;
        fpsLike << "Core: CHIP-8  CPU: " << (kCpuHz * cpuMultiplier) << "Hz  State: " << (running ? "Running" : "Paused");
        drawText(renderer, font, fpsLike.str(), 14, 370, SDL_Color{120, 149, 210, 255});
        drawText(renderer, font, statusText, 14, 386, SDL_Color{96, 118, 166, 255});

        SDL_RenderPresent(renderer);
    }

    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
