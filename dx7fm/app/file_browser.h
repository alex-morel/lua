#pragma once
#include "ui_widgets.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// [ADICIONADO em 2026-10-22, extraido de resynth_ui.h] Navegador minimo de
// arquivos .wav (pastas + .wav, sem ocultos), usado pelo dialogo de
// ressintese e pela importacao de ondas da Wavetable.
namespace ui {

struct WavBrowser {
    std::string dir;
    struct Entry { std::string name; bool isDir; };
    std::vector<Entry> entries;
    bool listed = false;
    char path[1024] = "";

    static bool isWav(const std::string& name) {
        if (name.size() < 4) return false;
        std::string ext = name.substr(name.size() - 4);
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext == ".wav";
    }

    void refresh() {
        namespace fs = std::filesystem;
        if (dir.empty()) {
            const char* home = std::getenv("HOME");
            dir = home ? home : ".";
        }
        entries.clear();
        std::error_code ec;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            std::error_code ec2;
            bool isDir = it->is_directory(ec2);
            if (isDir || isWav(name)) entries.push_back({name, isDir});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            if (a.isDir != b.isDir) return a.isDir;
            auto lower = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
            return lower(a.name) < lower(b.name);
        });
        listed = true;
    }

    // a pasta passa a ser a do arquivo aberto (proxima abertura comeca nela)
    void followFile(const std::string& file) {
        dir = std::filesystem::path(file).parent_path().string();
        listed = false;
    }

    // Lista com "^ acima" + pastas + .wav; devolve o caminho do .wav clicado
    // (vazio se nada). `current` fica destacado.
    std::string drawList(const char* id, ImVec2 size, const std::string& current) {
        namespace fs = std::filesystem;
        if (!listed) refresh();
        std::string picked;
        ImGui::BeginChild(id, size, ImGuiChildFlags_Borders);
        if (ImGui::SmallButton("^ acima")) {
            dir = fs::path(dir).parent_path().string();
            if (dir.empty()) dir = "/";
            listed = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", dir.c_str());
        ImGui::Separator();
        for (const auto& e : entries) {
            std::string label = (e.isDir ? "[pasta] " : "") + e.name;
            const std::string full = (fs::path(dir) / e.name).string();
            if (ImGui::Selectable(label.c_str(), !e.isDir && full == current)) {
                if (e.isDir) { dir = full; listed = false; }
                else { std::snprintf(path, sizeof path, "%s", full.c_str()); picked = full; }
            }
        }
        if (entries.empty()) ImGui::TextDisabled("(nenhuma pasta ou .wav aqui)");
        ImGui::EndChild();
        return picked;
    }

    // Campo de caminho + "Abrir"; devolve o caminho quando confirmado.
    std::string drawPathInput(const char* id) {
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(-80);
        bool enter = ImGui::InputTextWithHint("##path", "caminho de um arquivo .wav", path, sizeof path, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        bool open = ImGui::Button("Abrir", ImVec2(-1, 0)) || enter;
        ImGui::PopID();
        return open ? std::string(path) : std::string();
    }
};

} // namespace ui
