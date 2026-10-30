#pragma once
// [ADICIONADO em 2026-10-29] Rack de efeitos <-> JSON (presets de efeitos e o
// ultimo estado do app). Separado de dx7_patch_io.h pra quem so le patches nao
// puxar o DSP do rack.
//
// Formato (versao 1):
//   { "version": 1, "link": "Parallel",   (FxLink; arquivos antigos com
//     "serial": true viram "Serial")
//     "parts": [ {"dry": 0, "send1": 1, "send2": 0}, ... x4 ],
//     "chains": [ {"level": 1, "pan": 0,
//                  "slots": [ {"kind": "Hall", "on": true, "mix": 0.3,
//                              "params": {"decay": 3.2, "predelay": 20, ...}}, ... x4 ]}, ... x2 ],
//     "expression": {"chain": 0, "slot": 1, "param": "rate", "min": 0.5, "max": 6} }
// Efeitos e parametros vao POR NOME (FxInfo::id / FxParamInfo::id): reordenar a
// tabela nao quebra arquivos antigos. Efeito desconhecido vira slot vazio;
// parametro desconhecido e ignorado e os ausentes ficam no padrao do efeito.
#include "dx7_fxrack.h"
#include "dx7_patch_io.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace dx7fm {

inline JsonValue fxRackToJson(const FxRackParams& r) {
    JsonValue j = JsonValue::makeObject();
    j.set("version", JsonValue::makeNumber(1));
    j.set("link", JsonValue::makeString(kFxLinks[r.link < kFxNumLinks ? r.link : 0].id));
    JsonValue parts = JsonValue::makeArray();
    for (int p = 0; p < kFxParts; ++p) {
        JsonValue e = JsonValue::makeObject();
        e.set("dry", JsonValue::makeNumber(r.dry[p]));
        e.set("send1", JsonValue::makeNumber(r.send[p][0]));
        e.set("send2", JsonValue::makeNumber(r.send[p][1]));
        parts.push(std::move(e));
    }
    j.set("parts", std::move(parts));
    JsonValue chains = JsonValue::makeArray();
    for (const auto& c : r.chain) {
        JsonValue cj = JsonValue::makeObject();
        cj.set("level", JsonValue::makeNumber(c.level));
        cj.set("pan", JsonValue::makeNumber(c.pan));
        cj.set("algorithm", JsonValue::makeString(kFxChainAlgorithms[c.algorithm < kFxNumChainAlgorithms ? c.algorithm : 0].id));
        JsonValue slots = JsonValue::makeArray();
        for (const auto& s : c.slot) {
            const FxInfo& info = fxInfo(s.kind);
            JsonValue sj = JsonValue::makeObject();
            sj.set("kind", JsonValue::makeString(info.id));
            sj.set("on", JsonValue::makeBool(s.on));
            sj.set("mix", JsonValue::makeNumber(s.mix));
            JsonValue pj = JsonValue::makeObject();
            for (int i = 0; i < info.nParams; ++i) pj.set(info.params[i].id, JsonValue::makeNumber(s.p[i]));
            sj.set("params", std::move(pj));
            slots.push(std::move(sj));
        }
        cj.set("slots", std::move(slots));
        chains.push(std::move(cj));
    }
    j.set("chains", std::move(chains));
    if (r.expr.chain >= 0) {
        const FxInfo& info = fxInfo(r.chain[r.expr.chain].slot[r.expr.slot].kind);
        JsonValue e = JsonValue::makeObject();
        e.set("chain", JsonValue::makeNumber(r.expr.chain));
        e.set("slot", JsonValue::makeNumber(r.expr.slot));
        e.set("param", JsonValue::makeString(r.expr.param < info.nParams ? info.params[r.expr.param].id : ""));
        e.set("min", JsonValue::makeNumber(r.expr.min));
        e.set("max", JsonValue::makeNumber(r.expr.max));
        j.set("expression", std::move(e));
    }
    return j;
}

// Campos ausentes/invalidos ficam no padrao; o resultado ja sai sanitizado.
inline FxRackParams fxRackFromJson(const JsonValue& j) {
    FxRackParams r;
    const int link = fxLinkFromId(j.stringAt("link", ""));
    r.link = static_cast<uint8_t>(link >= 0 ? link : j.boolAt("serial", false) ? kFxLinkSerial : kFxLinkParallel);
    if (auto* parts = j.find("parts"))
        for (size_t p = 0; p < parts->arraySize() && p < static_cast<size_t>(kFxParts); ++p) {
            const JsonValue& e = parts->at(p);
            r.dry[p] = clampCast<float>(e.numberAt("dry", r.dry[p]));
            r.send[p][0] = clampCast<float>(e.numberAt("send1", r.send[p][0]));
            r.send[p][1] = clampCast<float>(e.numberAt("send2", r.send[p][1]));
        }
    if (auto* chains = j.find("chains"))
        for (size_t c = 0; c < chains->arraySize() && c < static_cast<size_t>(kFxChains); ++c) {
            const JsonValue& cj = chains->at(c);
            FxChainParams& ch = r.chain[c];
            ch.level = clampCast<float>(cj.numberAt("level", 1.0));
            ch.pan = clampCast<float>(cj.numberAt("pan", 0.0));
            const int alg = fxChainAlgorithmFromId(cj.stringAt("algorithm", "Serial"));
            ch.algorithm = static_cast<uint8_t>(alg >= 0 ? alg : 0);
            const JsonValue* slots = cj.find("slots");
            if (!slots) continue;
            for (size_t s = 0; s < slots->arraySize() && s < static_cast<size_t>(kFxSlots); ++s) {
                const JsonValue& sj = slots->at(s);
                FxSlotParams& sp = ch.slot[s];
                sp.setKind(fxKindFromId(sj.stringAt("kind", "Off")));
                sp.on = sj.boolAt("on", true);
                sp.mix = clampCast<float>(sj.numberAt("mix", sp.mix));
                const FxInfo& info = fxInfo(sp.kind);
                if (const JsonValue* pj = sj.find("params"))
                    for (int i = 0; i < info.nParams; ++i) sp.p[i] = clampCast<float>(pj->numberAt(info.params[i].id, sp.p[i]));
            }
        }
    if (const JsonValue* e = j.find("expression")) {
        int c = static_cast<int>(e->numberAt("chain", -1)), s = static_cast<int>(e->numberAt("slot", 0));
        if (c >= 0 && c < kFxChains && s >= 0 && s < kFxSlots) {
            const FxInfo& info = fxInfo(r.chain[c].slot[s].kind);
            std::string pid = e->stringAt("param", "");
            for (int i = 0; i < info.nParams; ++i)
                if (pid == info.params[i].id) {
                    r.expr.chain = c;
                    r.expr.slot = s;
                    r.expr.param = i;
                    r.expr.min = clampCast<float>(e->numberAt("min", info.params[i].min));
                    r.expr.max = clampCast<float>(e->numberAt("max", info.params[i].max));
                }
        }
    }
    sanitize(r);
    return r;
}

inline std::string fxRackToString(const FxRackParams& r) { return fxRackToJson(r).dump(0); }
inline FxRackParams fxRackFromString(const std::string& text) { return fxRackFromJson(JsonValue::parse(text)); }

inline bool saveFxRackFile(const std::filesystem::path& path, const FxRackParams& r) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path);
    if (!f) return false;
    f << fxRackToString(r) << "\n";
    return static_cast<bool>(f);
}

inline bool loadFxRackFile(const std::filesystem::path& path, FxRackParams& r) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    JsonValue j = JsonValue::parse(ss.str());
    if (!j.find("chains")) return false; // nao e um arquivo de rack
    r = fxRackFromJson(j);
    return true;
}

} // namespace dx7fm
