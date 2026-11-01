// [ADICIONADO em 2026-10-29] Screenshot da UI sem tela (SDL offscreen +
// renderer por software), pra conferir telas novas e documentar. Roda a UI
// real (app/main.cpp), sem audio nem MIDI. Use XDG_CONFIG_HOME apontando pra
// uma pasta temporaria pra nao mexer no settings.json de verdade.
//
//   SDL_VIDEODRIVER=offscreen ./build/ui_shot saida.bmp [passos...]
//
// passos (em ordem; cada um roda alguns quadros):
//   click:X,Y      clique do mouse em (X, Y) (janela 1440x900)
//   key:NOME       tecla (Escape, Enter, Tab, Down, Up)
//   text:TEXTO     digita texto no campo com foco
//   frames:N       roda N quadros
//   load:NOME      carrega o patch de fabrica NOME na parte A
//   fx:NOME        carrega o preset de efeitos de fabrica NOME e abre a aba Efeitos
//   tab:N          abre a aba N (0-3 = partes, 4 = Efeitos)
//   drag:X1,Y1,X2,Y2  arrasta com o botao esquerdo
//   rclick:X,Y     clique com o botao direito
//   ctrlz          Ctrl+Z
//   wheel:N        roda do mouse N passos pra baixo, com o mouse na lateral direita
#define main dx7fm_app_main
#include "../app/main.cpp"
#undef main

namespace {

int gForceTab = -1; // um quadro so

void frame(AppState& app, SDL_Renderer* renderer) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) ImGui_ImplSDL2_ProcessEvent(&e);
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    DrawMainUi(app, gForceTab);
    gForceTab = -1;
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 18, 20, 26, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
}

ImGuiKey keyNamed(const std::string& n) {
    if (n == "Escape") return ImGuiKey_Escape;
    if (n == "Enter") return ImGuiKey_Enter;
    if (n == "Tab") return ImGuiKey_Tab;
    if (n == "Down") return ImGuiKey_DownArrow;
    if (n == "Up") return ImGuiKey_UpArrow;
    return ImGuiKey_None;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "uso: ui_shot saida.bmp [click:X,Y | key:NOME | text:T | frames:N | load:NOME]...\n"); return 2; }
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Mare Tranquillitatis", 0, 0, 1440, 900, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    SetupAppStyle();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    auto* appPtr = new AppState; // grande: no heap
    AppState& app = *appPtr;
    initAppState(app);
    app.synth.setWorkerThreads(0);
    for (int i = 0; i < 3; ++i) frame(app, renderer);

    ImGuiIO& io = ImGui::GetIO();
    for (int a = 2; a < argc; ++a) {
        const std::string step = argv[a];
        const auto colon = step.find(':');
        const std::string kind = step.substr(0, colon), arg = colon == std::string::npos ? "" : step.substr(colon + 1);
        if (kind == "click") {
            const float x = std::stof(arg.substr(0, arg.find(','))), y = std::stof(arg.substr(arg.find(',') + 1));
            io.AddMousePosEvent(x, y);
            frame(app, renderer);
            io.AddMouseButtonEvent(0, true);
            frame(app, renderer);
            io.AddMouseButtonEvent(0, false);
            frame(app, renderer);
        } else if (kind == "key") {
            io.AddKeyEvent(keyNamed(arg), true);
            frame(app, renderer);
            io.AddKeyEvent(keyNamed(arg), false);
        } else if (kind == "text") {
            io.AddInputCharactersUTF8(arg.c_str());
        } else if (kind == "frames") {
            for (int i = 0; i < std::stoi(arg); ++i) frame(app, renderer);
            continue;
        } else if (kind == "load") {
            for (int i = 0; i < static_cast<int>(app.factory.size()); ++i)
                if (app.factory[static_cast<size_t>(i)].part.name == arg) loadFactoryPreset(app, 0, i);
        } else if (kind == "fx") {
            for (const auto& pr : app.fxFactory)
                if (pr.name == arg) dx7fm_app::fxui::loadPresetInto(app.fxUi, app.synth, pr.rack, pr.name, 0.0);
            gForceTab = 4;
        } else if (kind == "drag") {
            float v[4];
            std::sscanf(arg.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
            io.AddMousePosEvent(v[0], v[1]);
            frame(app, renderer);
            io.AddMouseButtonEvent(0, true);
            frame(app, renderer);
            for (int k = 1; k <= 8; ++k) {
                io.AddMousePosEvent(v[0] + (v[2] - v[0]) * static_cast<float>(k) / 8.0f, v[1] + (v[3] - v[1]) * static_cast<float>(k) / 8.0f);
                frame(app, renderer);
            }
            io.AddMouseButtonEvent(0, false);
            frame(app, renderer);
        } else if (kind == "ctrlz") {
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(ImGuiKey_LeftCtrl, true);
            frame(app, renderer);
            io.AddKeyEvent(ImGuiKey_Z, true);
            frame(app, renderer);
            io.AddKeyEvent(ImGuiKey_Z, false);
            io.AddKeyEvent(ImGuiKey_LeftCtrl, false);
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
        } else if (kind == "rclick") {
            const float x = std::stof(arg.substr(0, arg.find(','))), y = std::stof(arg.substr(arg.find(',') + 1));
            io.AddMousePosEvent(x, y);
            frame(app, renderer);
            io.AddMouseButtonEvent(1, true);
            frame(app, renderer);
            io.AddMouseButtonEvent(1, false);
            frame(app, renderer);
        } else if (kind == "wheel") {
            io.AddMousePosEvent(1400.0f, 500.0f);
            frame(app, renderer);
            for (int i = 0; i < std::stoi(arg); ++i) { io.AddMouseWheelEvent(0.0f, -1.0f); frame(app, renderer); }
        } else if (kind == "tab") {
            gForceTab = std::stoi(arg);
        }
        for (int i = 0; i < 3; ++i) frame(app, renderer);
    }

    // le o que foi desenhado no ultimo quadro
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    DrawMainUi(app);
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 18, 20, 26, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, 1440, 900, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch);
    SDL_SaveBMP(shot, argv[1]);
    std::printf("salvo %s\n", argv[1]);
    std::fflush(stdout);
    std::_Exit(0); // sem destrutores: o app nao foi feito pra desmontar no meio
}
