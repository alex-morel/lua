#pragma once
// [ADICIONADO em 2026-11-01] Sequencer <-> JSON (padroes salvos e ultimo estado).
#include "dx7_patch_io.h"
#include "dx7_sequencer.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace dx7fm {

inline std::string seqToString(const SeqParams& p) {
    JsonValue j = JsonValue::makeObject();
    j.set("kind", JsonValue::makeString("sequence"));
    j.set("version", JsonValue::makeNumber(1));
    JsonValue tracks = JsonValue::makeArray();
    for (const auto& t : p.track) {
        JsonValue tj = JsonValue::makeObject();
        tj.set("mute", JsonValue::makeBool(t.mute));
        tj.set("length", JsonValue::makeNumber(t.length));
        tj.set("division", JsonValue::makeString(lfoDivisionName(t.division)));
        tj.set("swing", JsonValue::makeNumber(t.swing));
        JsonValue steps = JsonValue::makeArray();
        for (const auto& s : t.step) {
            JsonValue sj = JsonValue::makeObject();
            sj.set("on", JsonValue::makeBool(s.on));
            sj.set("note", JsonValue::makeNumber(s.note));
            sj.set("velocity", JsonValue::makeNumber(s.velocity));
            sj.set("gate", JsonValue::makeNumber(s.gate));
            sj.set("mod", JsonValue::makeNumber(s.mod));
            steps.push(std::move(sj));
        }
        tj.set("steps", std::move(steps));
        tracks.push(std::move(tj));
    }
    j.set("tracks", std::move(tracks));
    return j.dump();
}

// false = nao e um padrao do sequencer. Valores saneados.
inline bool seqFromString(const std::string& text, SeqParams& out) {
    JsonValue j = JsonValue::parse(text);
    if (j.stringAt("kind", "") != "sequence") return false;
    SeqParams p;
    if (const JsonValue* tracks = j.find("tracks"))
        for (size_t k = 0; k < tracks->arraySize() && k < static_cast<size_t>(kSeqTracks); ++k) {
            const JsonValue& tj = tracks->at(k);
            SeqTrack& t = p.track[k];
            t.mute = tj.boolAt("mute", false);
            t.length = clampCast<uint8_t>(tj.numberAt("length", 16));
            const std::string d = tj.stringAt("division", lfoDivisionName(t.division));
            for (int v = 0; v < kNumLfoDivisions; ++v) if (d == lfoDivisionName(v)) t.division = static_cast<uint8_t>(v);
            t.swing = clampCast<float>(tj.numberAt("swing", 0.0));
            if (const JsonValue* steps = tj.find("steps"))
                for (size_t i = 0; i < steps->arraySize() && i < static_cast<size_t>(kSeqSteps); ++i) {
                    const JsonValue& sj = steps->at(i);
                    SeqStep& s = t.step[i];
                    s.on = sj.boolAt("on", false);
                    s.note = clampCast<uint8_t>(sj.numberAt("note", 60));
                    s.velocity = clampCast<uint8_t>(sj.numberAt("velocity", 100));
                    s.gate = clampCast<float>(sj.numberAt("gate", 0.5));
                    s.mod = clampCast<float>(sj.numberAt("mod", 0.0));
                }
        }
    sanitize(p);
    out = p;
    return true;
}

inline bool saveSeqFile(const std::filesystem::path& path, const SeqParams& p) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path);
    if (!f) return false;
    f << seqToString(p) << "\n";
    return static_cast<bool>(f);
}
inline bool loadSeqFile(const std::filesystem::path& path, SeqParams& p) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return seqFromString(ss.str(), p);
}

} // namespace dx7fm
