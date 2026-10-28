#pragma once
#include "dx7_params.h"
#include "dx7_patch_io.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

// [ADICIONADO em 2026-10-24] Configuracoes do APP (nao do patch), em
// $XDG_CONFIG_HOME/dx7fm/settings.json (ou ~/.config/dx7fm/settings.json):
// oversampling, modo do clock, mapeamentos de CC (MIDI learn) e os ultimos
// arquivos de afinacao de cada parte. Usa o JSON de dx7_patch_io.h.
namespace dx7fm_app {

// Alvo de um CC aprendido
enum class CcTarget : int { Macro1, Macro2, Macro3, Macro4, MasterVolume, Bpm };
inline const char* ccTargetName(CcTarget t) {
    switch (t) {
        case CcTarget::Macro1: return "Macro1";
        case CcTarget::Macro2: return "Macro2";
        case CcTarget::Macro3: return "Macro3";
        case CcTarget::Macro4: return "Macro4";
        case CcTarget::MasterVolume: return "Volume";
        case CcTarget::Bpm: return "BPM";
    }
    return "Macro1";
}

struct CcMapping {
    int cc = 0;          // 0..127
    CcTarget target = CcTarget::Macro1;
};

struct AppSettings {
    int oversample = 1;      // 1, 2 ou 4 (Synth::init)
    bool midiClock = true;   // true: BPM segue o clock MIDI quando ele chega
    std::vector<CcMapping> cc;
    std::string sclPath[4], kbmPath[4]; // por parte (A..D)
    bool mpe = false;            // [2026-10-28] MPE zona inferior (mpe.h)
    float mpeBendRange = 48.0f;  // bend por nota, semitons

    int ccFor(CcTarget t) const {
        for (const auto& m : cc) if (m.target == t) return m.cc;
        return -1;
    }
    void learn(CcTarget t, int ccNumber) { // um CC por alvo e um alvo por CC
        forget(t);
        for (size_t i = 0; i < cc.size();) { if (cc[i].cc == ccNumber) cc.erase(cc.begin() + static_cast<long>(i)); else ++i; }
        cc.push_back({ccNumber, t});
    }
    void forget(CcTarget t) {
        for (size_t i = 0; i < cc.size();) { if (cc[i].target == t) cc.erase(cc.begin() + static_cast<long>(i)); else ++i; }
    }
};

inline std::filesystem::path settingsPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path base = xdg && *xdg ? std::filesystem::path(xdg) : std::filesystem::path(home ? home : ".") / ".config";
    return base / "dx7fm" / "settings.json";
}

inline std::string settingsToJson(const AppSettings& s) {
    using dx7fm::JsonValue;
    JsonValue j = JsonValue::makeObject();
    j.set("oversample", JsonValue::makeNumber(s.oversample));
    j.set("midiClock", JsonValue::makeBool(s.midiClock));
    JsonValue cc = JsonValue::makeArray();
    for (const auto& m : s.cc) {
        JsonValue e = JsonValue::makeObject();
        e.set("cc", JsonValue::makeNumber(m.cc));
        e.set("target", JsonValue::makeString(ccTargetName(m.target)));
        cc.push(std::move(e));
    }
    j.set("cc", std::move(cc));
    JsonValue tun = JsonValue::makeArray();
    for (int p = 0; p < 4; ++p) {
        JsonValue e = JsonValue::makeObject();
        e.set("scl", JsonValue::makeString(s.sclPath[p]));
        e.set("kbm", JsonValue::makeString(s.kbmPath[p]));
        tun.push(std::move(e));
    }
    j.set("tuning", std::move(tun));
    j.set("mpe", JsonValue::makeBool(s.mpe));
    j.set("mpeBendRange", JsonValue::makeNumber(s.mpeBendRange));
    return j.dump();
}

inline AppSettings settingsFromJson(const std::string& text) {
    using dx7fm::JsonValue;
    AppSettings s;
    JsonValue j = JsonValue::parse(text);
    int os = static_cast<int>(j.numberAt("oversample", 1));
    s.oversample = os == 2 || os == 4 ? os : 1;
    s.midiClock = j.boolAt("midiClock", true);
    s.mpe = j.boolAt("mpe", false);
    s.mpeBendRange = std::clamp(static_cast<float>(j.numberAt("mpeBendRange", 48.0)), 1.0f, 96.0f);
    if (auto* cc = j.find("cc"))
        for (size_t i = 0; i < cc->arraySize(); ++i) {
            const JsonValue& e = cc->at(i);
            int n = static_cast<int>(e.numberAt("cc", -1));
            std::string t = e.stringAt("target", "");
            for (int k = 0; k <= static_cast<int>(CcTarget::Bpm); ++k)
                if (t == ccTargetName(static_cast<CcTarget>(k)) && n >= 0 && n < 128) s.learn(static_cast<CcTarget>(k), n);
        }
    if (auto* tun = j.find("tuning"))
        for (size_t p = 0; p < tun->arraySize() && p < 4; ++p) {
            s.sclPath[p] = tun->at(p).stringAt("scl", "");
            s.kbmPath[p] = tun->at(p).stringAt("kbm", "");
        }
    return s;
}

inline AppSettings loadSettings(const std::filesystem::path& path = settingsPath()) {
    std::ifstream f(path);
    if (!f) return AppSettings{};
    std::stringstream ss;
    ss << f.rdbuf();
    return settingsFromJson(ss.str());
}

inline bool saveSettings(const AppSettings& s, const std::filesystem::path& path = settingsPath()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path);
    if (!f) return false;
    f << settingsToJson(s) << "\n";
    return static_cast<bool>(f);
}

} // namespace dx7fm_app
