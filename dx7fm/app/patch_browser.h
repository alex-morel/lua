#pragma once
#include "dx7_params.h"
#include "dx7_patch_io.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// [ADICIONADO em 2026-09-23] Logica do navegador de patches do app (lista,
// nome de arquivo, o que um "patch" carrega). Separada da UI (main.cpp) e
// sem SDL/ALSA, pra ser testada no demo.cpp.
namespace dx7fm_app {

struct PatchEntry {
    std::string stem;            // nome exibido (nome do arquivo sem .json)
    std::filesystem::path path;
};

// Lista os .json de `dir`, ordenados por nome sem diferenciar maiusculas.
// Diretorio inexistente (ainda nao salvou nada) = lista vazia, nao erro.
inline std::vector<PatchEntry> listPatches(const std::filesystem::path& dir) {
    std::vector<PatchEntry> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
        out.push_back({it->path().stem().string(), it->path()});
    }
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::sort(out.begin(), out.end(), [&](const PatchEntry& a, const PatchEntry& b) { return lower(a.stem) < lower(b.stem); });
    return out;
}

// Nome do patch -> nome de arquivo seguro: troca separadores de caminho e
// caracteres problematicos por '_', tira espacos/pontos das pontas (".."
// nao pode escapar do diretorio). Acentos (UTF-8) passam intactos.
inline std::string safeFileStem(const std::string& name) {
    std::string s;
    for (unsigned char c : name) {
        bool bad = c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                   c == '"' || c == '<' || c == '>' || c == '|';
        s += bad ? '_' : static_cast<char>(c);
    }
    size_t b = s.find_first_not_of(" .");
    size_t e = s.find_last_not_of(" .");
    s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    return s.empty() ? "sem_nome" : s;
}

inline std::filesystem::path patchPathFor(const std::filesystem::path& dir, const std::string& name) {
    return dir / (safeFileStem(name) + ".json");
}

// Um "patch" e o SOM da parte (timbre, modo de voz, glide, unison, bend,
// transpose, nome). O ROTEAMENTO/MIX da parte fica como esta: habilitada,
// canal MIDI, faixas de tecla/velocity, vozes reservadas, nivel e pan --
// carregar um patch na Parte B nao pode desligar a B, mudar o canal dela
// ou o split do teclado (antes, o load substituia a parte inteira).
inline void copyPatchKeepingRouting(dx7fm::PartParams& dst, const dx7fm::PartParams& src) {
    dx7fm::PartParams keep = dst;
    dst = src;
    dst.enabled = keep.enabled;
    dst.midiChannel = keep.midiChannel;
    dst.loKey = keep.loKey;
    dst.hiKey = keep.hiKey;
    dst.loVel = keep.loVel;
    dst.hiVel = keep.hiVel;
    dst.reservedVoices = keep.reservedVoices;
    dst.level = keep.level;
    dst.pan = keep.pan;
}

// Impressao digital do SOM da parte (ignora o roteamento, ver acima): duas
// partes com o mesmo fingerprint soam igual. Usado pra detectar alteracoes
// nao salvas -- desfazer uma edicao volta a "nao modificado".
inline std::string patchFingerprint(const dx7fm::PartParams& p) {
    dx7fm::PartParams normalized;
    copyPatchKeepingRouting(normalized, p);
    return dx7fm::partParamsToJson(normalized).dump();
}

} // namespace dx7fm_app
