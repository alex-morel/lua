#pragma once
#include "dx7_params.h"
#include <cctype>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dx7fm {

// Valor JSON minimo, generico o bastante pra serializar qualquer VoiceParams/
// PartParams sem depender de lib externa (o projeto so usa STL, ver
// ARQUITETURA.txt secao 2). NAO e um parser JSON de proposito geral robusto
// contra qualquer entrada malformada -- assume que o texto foi gerado por
// dump() ou editado a mao com cuidado. Campos ausentes/invalidos caem no
// default do struct (ver *FromJson abaixo), entao arquivos de versoes
// antigas com menos campos continuam carregando.
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;

    static JsonValue makeNull() { return JsonValue(); }
    static JsonValue makeBool(bool b) { JsonValue v; v.type_ = Type::Bool; v.bool_ = b; return v; }
    static JsonValue makeNumber(double n) { JsonValue v; v.type_ = Type::Number; v.num_ = n; return v; }
    static JsonValue makeString(std::string s) { JsonValue v; v.type_ = Type::String; v.str_ = std::move(s); return v; }
    static JsonValue makeArray() { JsonValue v; v.type_ = Type::Array; return v; }
    static JsonValue makeObject() { JsonValue v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }

    void set(const std::string& key, JsonValue v) { obj_.emplace_back(key, std::move(v)); }
    void push(JsonValue v) { arr_.push_back(std::move(v)); }

    bool asBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double asNumber(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
    std::string asString(const std::string& def = "") const { return type_ == Type::String ? str_ : def; }

    const JsonValue* find(const std::string& key) const {
        for (auto& kv : obj_) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    size_t arraySize() const { return arr_.size(); }
    const JsonValue& at(size_t i) const { return arr_[i]; }

    double numberAt(const std::string& key, double def) const { auto* v = find(key); return v ? v->asNumber(def) : def; }
    bool boolAt(const std::string& key, bool def) const { auto* v = find(key); return v ? v->asBool(def) : def; }
    std::string stringAt(const std::string& key, const std::string& def) const { auto* v = find(key); return v ? v->asString(def) : def; }

    std::string dump(int indent = 0) const {
        std::string pad(static_cast<size_t>(indent) * 2, ' ');
        std::string padIn(static_cast<size_t>(indent + 1) * 2, ' ');
        switch (type_) {
            case Type::Null: return "null";
            case Type::Bool: return bool_ ? "true" : "false";
            case Type::Number: {
                if (!std::isfinite(num_)) return "null"; // JSON nao tem NaN/Inf
                std::ostringstream oss;
                // checa a faixa antes do cast: long long de um double enorme e UB
                if (std::fabs(num_) < 9.0e15 && num_ == static_cast<double>(static_cast<long long>(num_))) {
                    oss << static_cast<long long>(num_);
                } else {
                    oss.precision(9);
                    oss << num_;
                }
                return oss.str();
            }
            case Type::String: return "\"" + escape(str_) + "\"";
            case Type::Array: {
                if (arr_.empty()) return "[]";
                std::string out = "[\n";
                for (size_t i = 0; i < arr_.size(); ++i) {
                    out += padIn + arr_[i].dump(indent + 1);
                    if (i + 1 < arr_.size()) out += ",";
                    out += "\n";
                }
                out += pad + "]";
                return out;
            }
            case Type::Object: {
                if (obj_.empty()) return "{}";
                std::string out = "{\n";
                for (size_t i = 0; i < obj_.size(); ++i) {
                    out += padIn + "\"" + escape(obj_[i].first) + "\": " + obj_[i].second.dump(indent + 1);
                    if (i + 1 < obj_.size()) out += ",";
                    out += "\n";
                }
                out += pad + "}";
                return out;
            }
        }
        return "null";
    }

    static JsonValue parse(const std::string& text) {
        size_t pos = 0;
        return parseValue(text, pos);
    }

private:
    static std::string escape(const std::string& s) {
        std::string out;
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                default:   out += c;
            }
        }
        return out;
    }

    static void skipWs(const std::string& t, size_t& p) {
        while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) ++p;
    }

    static JsonValue parseValue(const std::string& t, size_t& p) {
        skipWs(t, p);
        if (p >= t.size()) return JsonValue();
        char c = t[p];
        if (c == '{') return parseObject(t, p);
        if (c == '[') return parseArray(t, p);
        if (c == '"') return makeString(parseString(t, p));
        if (c == 't' && t.compare(p, 4, "true") == 0) { p += 4; return makeBool(true); }
        if (c == 'f' && t.compare(p, 5, "false") == 0) { p += 5; return makeBool(false); }
        if (c == 'n' && t.compare(p, 4, "null") == 0) { p += 4; return JsonValue(); }
        return parseNumber(t, p);
    }

    static JsonValue parseObject(const std::string& t, size_t& p) {
        JsonValue v = makeObject();
        ++p; // {
        skipWs(t, p);
        if (p < t.size() && t[p] == '}') { ++p; return v; }
        while (p < t.size()) {
            skipWs(t, p);
            std::string key = parseString(t, p);
            skipWs(t, p);
            if (p < t.size() && t[p] == ':') ++p;
            JsonValue val = parseValue(t, p);
            v.obj_.emplace_back(std::move(key), std::move(val));
            skipWs(t, p);
            if (p < t.size() && t[p] == ',') { ++p; continue; }
            if (p < t.size() && t[p] == '}') { ++p; break; }
            break;
        }
        return v;
    }

    static JsonValue parseArray(const std::string& t, size_t& p) {
        JsonValue v = makeArray();
        ++p; // [
        skipWs(t, p);
        if (p < t.size() && t[p] == ']') { ++p; return v; }
        while (p < t.size()) {
            v.arr_.push_back(parseValue(t, p));
            skipWs(t, p);
            if (p < t.size() && t[p] == ',') { ++p; continue; }
            if (p < t.size() && t[p] == ']') { ++p; break; }
            break;
        }
        return v;
    }

    static std::string parseString(const std::string& t, size_t& p) {
        std::string out;
        if (p >= t.size() || t[p] != '"') return out;
        ++p;
        while (p < t.size() && t[p] != '"') {
            char c = t[p];
            if (c == '\\' && p + 1 < t.size()) {
                char n = t[p + 1];
                switch (n) {
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    default:   out += n;
                }
                p += 2;
            } else {
                out += c;
                ++p;
            }
        }
        if (p < t.size()) ++p; // fecha "
        return out;
    }

    static JsonValue parseNumber(const std::string& t, size_t& p) {
        size_t start = p;
        if (p < t.size() && (t[p] == '-' || t[p] == '+')) ++p;
        while (p < t.size() && (std::isdigit(static_cast<unsigned char>(t[p])) ||
                                 t[p] == '.' || t[p] == 'e' || t[p] == 'E' || t[p] == '-' || t[p] == '+')) ++p;
        double n = 0.0;
        try { n = std::stod(t.substr(start, p - start)); } catch (...) { n = 0.0; }
        return makeNumber(n);
    }

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<JsonValue> arr_;
    std::vector<std::pair<std::string, JsonValue>> obj_;
};

// Converte numero do JSON pro tipo do campo SEM UB: static_cast<uint8_t>(300.0),
// de NaN, ou static_cast<float>(1e40) e comportamento indefinido; aqui satura
// na faixa do tipo (NaN/Inf viram 0 -- sanitize() ajusta pra faixa util depois).
template <typename T>
inline T clampCast(double d) {
    if (!std::isfinite(d)) return T{};
    double lo = static_cast<double>(std::numeric_limits<T>::lowest());
    double hi = static_cast<double>(std::numeric_limits<T>::max());
    return static_cast<T>(std::clamp(d, lo, hi));
}

// --- enums <-> string (formato legivel/estavel entre versoes) ----------

inline const char* waveformName(Waveform w) {
    switch (w) {
        case Waveform::Sine:            return "Sine";
        case Waveform::Triangle:        return "Triangle";
        case Waveform::Saw:             return "Saw";
        case Waveform::Square:          return "Square";
        case Waveform::Noise:           return "Noise";
        case Waveform::HalfSine:        return "HalfSine";
        case Waveform::AbsSine:         return "AbsSine";
        case Waveform::QuarterSine:     return "QuarterSine";
        case Waveform::AlternatingSine: return "AlternatingSine";
        case Waveform::CamelSine:       return "CamelSine";
        case Waveform::SquaredSine:     return "SquaredSine";
        case Waveform::LogSaw:          return "LogSaw";
        case Waveform::Additive:        return "Additive";
    }
    return "Sine";
}
inline Waveform waveformFromName(const std::string& s) {
    if (s == "Triangle")        return Waveform::Triangle;
    if (s == "Saw")             return Waveform::Saw;
    if (s == "Square")          return Waveform::Square;
    if (s == "Noise")           return Waveform::Noise;
    if (s == "HalfSine")        return Waveform::HalfSine;
    if (s == "AbsSine")         return Waveform::AbsSine;
    if (s == "QuarterSine")     return Waveform::QuarterSine;
    if (s == "AlternatingSine") return Waveform::AlternatingSine;
    if (s == "CamelSine")       return Waveform::CamelSine;
    if (s == "SquaredSine")     return Waveform::SquaredSine;
    if (s == "LogSaw")          return Waveform::LogSaw;
    if (s == "Additive")        return Waveform::Additive;
    return Waveform::Sine;
}

inline const char* fxTypeName(FxType f) {
    switch (f) {
        case FxType::Bypass:    return "Bypass";
        case FxType::LowPass:   return "LowPass";
        case FxType::BandPass:  return "BandPass";
        case FxType::HighPass:  return "HighPass";
        case FxType::RingMod:   return "RingMod";
        case FxType::Saturate:  return "Saturate";
        case FxType::Crush:     return "Crush";
        case FxType::FreqShift: return "FreqShift";
        case FxType::Comb:      return "Comb";
        case FxType::Fold:      return "Fold";
    }
    return "Bypass";
}
inline FxType fxTypeFromName(const std::string& s) {
    if (s == "LowPass")   return FxType::LowPass;
    if (s == "BandPass")  return FxType::BandPass;
    if (s == "HighPass")  return FxType::HighPass;
    if (s == "RingMod")   return FxType::RingMod;
    if (s == "Saturate")  return FxType::Saturate;
    if (s == "Crush")     return FxType::Crush;
    if (s == "FreqShift") return FxType::FreqShift;
    if (s == "Comb")      return FxType::Comb;
    if (s == "Fold")      return FxType::Fold;
    return FxType::Bypass;
}

inline const char* lfoWaveName(LfoWave w) {
    switch (w) {
        case LfoWave::Triangle:   return "Triangle";
        case LfoWave::SawUp:      return "SawUp";
        case LfoWave::SawDown:    return "SawDown";
        case LfoWave::Square:     return "Square";
        case LfoWave::Sine:       return "Sine";
        case LfoWave::SampleHold: return "SampleHold";
    }
    return "Triangle";
}
inline LfoWave lfoWaveFromName(const std::string& s) {
    if (s == "SawUp")      return LfoWave::SawUp;
    if (s == "SawDown")    return LfoWave::SawDown;
    if (s == "Square")     return LfoWave::Square;
    if (s == "Sine")       return LfoWave::Sine;
    if (s == "SampleHold") return LfoWave::SampleHold;
    return LfoWave::Triangle;
}

inline const char* modSourceName(ModSource s) {
    switch (s) {
        case ModSource::None:      return "None";
        case ModSource::LFO1:      return "LFO1";
        case ModSource::LFO2:      return "LFO2";
        case ModSource::ModWheel:  return "ModWheel";
        case ModSource::ChannelAT: return "ChannelAT";
        case ModSource::PolyAT:    return "PolyAT";
        case ModSource::PitchBend: return "PitchBend";
        case ModSource::KeyNote:   return "KeyNote";
        case ModSource::Velocity:  return "Velocity";
    }
    return "None";
}
inline ModSource modSourceFromName(const std::string& s) {
    if (s == "LFO1")      return ModSource::LFO1;
    if (s == "LFO2")      return ModSource::LFO2;
    if (s == "ModWheel")  return ModSource::ModWheel;
    if (s == "ChannelAT") return ModSource::ChannelAT;
    if (s == "PolyAT")    return ModSource::PolyAT;
    if (s == "PitchBend") return ModSource::PitchBend;
    if (s == "KeyNote") return ModSource::KeyNote;
    if (s == "Velocity") return ModSource::Velocity;
    return ModSource::None;
}

inline const char* modDestName(ModDest d) {
    switch (d) {
        case ModDest::None:             return "None";
        case ModDest::OperatorLevel:    return "OperatorLevel";
        case ModDest::OperatorFx1:      return "OperatorFx1";
        case ModDest::OperatorFeedback: return "OperatorFeedback";
        case ModDest::VoicePitch:       return "VoicePitch";
        case ModDest::OperatorMorph:    return "OperatorMorph";
    }
    return "None";
}
inline ModDest modDestFromName(const std::string& s) {
    if (s == "OperatorLevel")    return ModDest::OperatorLevel;
    if (s == "OperatorFx1")      return ModDest::OperatorFx1;
    if (s == "OperatorFeedback") return ModDest::OperatorFeedback;
    if (s == "VoicePitch")       return ModDest::VoicePitch;
    if (s == "OperatorMorph")    return ModDest::OperatorMorph;
    return ModDest::None;
}

inline const char* voiceModeName(VoiceMode m) {
    switch (m) {
        case VoiceMode::Poly:   return "Poly";
        case VoiceMode::Mono:   return "Mono";
        case VoiceMode::Unison: return "Unison";
    }
    return "Poly";
}
inline VoiceMode voiceModeFromName(const std::string& s) {
    if (s == "Mono")   return VoiceMode::Mono;
    if (s == "Unison") return VoiceMode::Unison;
    return VoiceMode::Poly;
}

// --- structs de dx7_params.h <-> JSON -----------------------------------

inline JsonValue egParamsToJson(const EGParams& eg) {
    JsonValue v = JsonValue::makeObject();
    JsonValue rate = JsonValue::makeArray();
    JsonValue level = JsonValue::makeArray();
    for (int i = 0; i < 4; ++i) {
        rate.push(JsonValue::makeNumber(eg.rate[i]));
        level.push(JsonValue::makeNumber(eg.level[i]));
    }
    v.set("rate", std::move(rate));
    v.set("level", std::move(level));
    return v;
}
inline EGParams egParamsFromJson(const JsonValue* j) {
    EGParams eg;
    if (!j) return eg;
    if (auto* rate = j->find("rate"))
        for (int i = 0; i < 4 && i < static_cast<int>(rate->arraySize()); ++i)
            eg.rate[i] = clampCast<uint8_t>(rate->at(static_cast<size_t>(i)).asNumber(eg.rate[i]));
    if (auto* level = j->find("level"))
        for (int i = 0; i < 4 && i < static_cast<int>(level->arraySize()); ++i)
            eg.level[i] = clampCast<uint8_t>(level->at(static_cast<size_t>(i)).asNumber(eg.level[i]));
    return eg;
}

inline JsonValue pegParamsToJson(const PEGParams& p) {
    JsonValue v = JsonValue::makeObject();
    JsonValue rate = JsonValue::makeArray();
    JsonValue level = JsonValue::makeArray();
    for (int i = 0; i < 4; ++i) {
        rate.push(JsonValue::makeNumber(p.rate[i]));
        level.push(JsonValue::makeNumber(p.level[i]));
    }
    v.set("rate", std::move(rate));
    v.set("level", std::move(level));
    v.set("rangeOctaves", JsonValue::makeNumber(p.rangeOctaves));
    return v;
}
inline PEGParams pegParamsFromJson(const JsonValue* j) {
    PEGParams p;
    if (!j) return p;
    if (auto* rate = j->find("rate"))
        for (int i = 0; i < 4 && i < static_cast<int>(rate->arraySize()); ++i)
            p.rate[i] = clampCast<uint8_t>(rate->at(static_cast<size_t>(i)).asNumber(p.rate[i]));
    if (auto* level = j->find("level"))
        for (int i = 0; i < 4 && i < static_cast<int>(level->arraySize()); ++i)
            p.level[i] = clampCast<uint8_t>(level->at(static_cast<size_t>(i)).asNumber(p.level[i]));
    p.rangeOctaves = clampCast<float>(j->numberAt("rangeOctaves", p.rangeOctaves));
    return p;
}

inline JsonValue keyLevelScalingToJson(const KeyLevelScaling& k) {
    JsonValue v = JsonValue::makeObject();
    v.set("breakpoint", JsonValue::makeNumber(k.breakpoint));
    v.set("leftDepth", JsonValue::makeNumber(k.leftDepth));
    v.set("rightDepth", JsonValue::makeNumber(k.rightDepth));
    v.set("leftExp", JsonValue::makeBool(k.leftExp));
    v.set("rightExp", JsonValue::makeBool(k.rightExp));
    return v;
}
inline KeyLevelScaling keyLevelScalingFromJson(const JsonValue* j) {
    KeyLevelScaling k;
    if (!j) return k;
    k.breakpoint = clampCast<uint8_t>(j->numberAt("breakpoint", k.breakpoint));
    k.leftDepth = clampCast<int8_t>(j->numberAt("leftDepth", k.leftDepth));
    k.rightDepth = clampCast<int8_t>(j->numberAt("rightDepth", k.rightDepth));
    k.leftExp = j->boolAt("leftExp", k.leftExp);
    k.rightExp = j->boolAt("rightExp", k.rightExp);
    return k;
}

inline JsonValue lfoParamsToJson(const LFOParams& l) {
    JsonValue v = JsonValue::makeObject();
    v.set("rate", JsonValue::makeNumber(l.rate));
    v.set("wave", JsonValue::makeString(lfoWaveName(l.wave)));
    v.set("delaySec", JsonValue::makeNumber(l.delaySec));
    v.set("keySync", JsonValue::makeBool(l.keySync));
    v.set("pitchDepth", JsonValue::makeNumber(l.pitchDepth));
    v.set("ampDepth", JsonValue::makeNumber(l.ampDepth));
    return v;
}
inline LFOParams lfoParamsFromJson(const JsonValue* j) {
    LFOParams l;
    if (!j) return l;
    l.rate = clampCast<float>(j->numberAt("rate", l.rate));
    l.wave = lfoWaveFromName(j->stringAt("wave", lfoWaveName(l.wave)));
    l.delaySec = clampCast<float>(j->numberAt("delaySec", l.delaySec));
    l.keySync = j->boolAt("keySync", l.keySync);
    l.pitchDepth = clampCast<float>(j->numberAt("pitchDepth", l.pitchDepth));
    l.ampDepth = clampCast<float>(j->numberAt("ampDepth", l.ampDepth));
    return l;
}

// "additive": {"a": {"amp": [8], "ratio": [8]}, "b": {...}, "morph": x}
inline JsonValue partialSetToJson(const OperatorParams::PartialSet& p) {
    JsonValue v = JsonValue::makeObject();
    JsonValue amp = JsonValue::makeArray(), ratio = JsonValue::makeArray();
    for (int k = 0; k < kNumPartials; ++k) {
        amp.push(JsonValue::makeNumber(p.amp[k]));
        ratio.push(JsonValue::makeNumber(p.ratio[k]));
    }
    v.set("amp", std::move(amp));
    v.set("ratio", std::move(ratio));
    return v;
}
inline OperatorParams::PartialSet partialSetFromJson(const JsonValue* j) {
    OperatorParams::PartialSet p;
    if (!j) return p;
    if (auto* amp = j->find("amp"))
        for (int k = 0; k < kNumPartials && k < static_cast<int>(amp->arraySize()); ++k)
            p.amp[k] = clampCast<float>(amp->at(static_cast<size_t>(k)).asNumber(p.amp[k]));
    if (auto* ratio = j->find("ratio"))
        for (int k = 0; k < kNumPartials && k < static_cast<int>(ratio->arraySize()); ++k)
            p.ratio[k] = clampCast<float>(ratio->at(static_cast<size_t>(k)).asNumber(p.ratio[k]));
    return p;
}

inline JsonValue operatorParamsToJson(const OperatorParams& o) {
    JsonValue j = JsonValue::makeObject();
    j.set("fixedFreq", JsonValue::makeBool(o.fixedFreq));
    j.set("coarseRatio", JsonValue::makeNumber(o.coarseRatio));
    j.set("fineCents", JsonValue::makeNumber(o.fineCents));
    j.set("fixedHz", JsonValue::makeNumber(o.fixedHz));
    j.set("detuneCents", JsonValue::makeNumber(o.detuneCents));
    j.set("outputLevel", JsonValue::makeNumber(o.outputLevel));
    j.set("ampEG", egParamsToJson(o.ampEG));
    j.set("fxEG", egParamsToJson(o.fxEG));
    j.set("levelScaling", keyLevelScalingToJson(o.levelScaling));
    j.set("rateScaling", JsonValue::makeNumber(o.rateScaling));
    j.set("velocitySens", JsonValue::makeNumber(o.velocitySens));
    j.set("lfoAmpModSens", JsonValue::makeNumber(o.lfoAmpModSens));
    j.set("lfoPitchModSens", JsonValue::makeNumber(o.lfoPitchModSens));
    j.set("feedback", JsonValue::makeNumber(o.feedback));
    j.set("waveform", JsonValue::makeString(waveformName(o.waveform)));
    j.set("phaseDistortion", JsonValue::makeNumber(o.phaseDistortion));
    j.set("fxType", JsonValue::makeString(fxTypeName(o.fxType)));
    j.set("fxParam1", JsonValue::makeNumber(o.fxParam1));
    j.set("fxParam2", JsonValue::makeNumber(o.fxParam2));
    j.set("fxKeyTrack", JsonValue::makeNumber(o.fxKeyTrack));
    j.set("ringModSource", JsonValue::makeNumber(o.ringModSource));
    j.set("fxEGReleaseWithAmp", JsonValue::makeBool(o.fxEGReleaseWithAmp));
    j.set("pegDepth", JsonValue::makeNumber(o.pegDepth));
    j.set("pegAffectsFixed", JsonValue::makeBool(o.pegAffectsFixed));
    JsonValue add = JsonValue::makeObject();
    add.set("a", partialSetToJson(o.additiveA));
    add.set("b", partialSetToJson(o.additiveB));
    add.set("morph", JsonValue::makeNumber(o.morphDepth));
    j.set("additive", std::move(add));
    return j;
}
inline OperatorParams operatorParamsFromJson(const JsonValue* j) {
    OperatorParams o;
    if (!j) return o;
    o.fixedFreq = j->boolAt("fixedFreq", o.fixedFreq);
    o.coarseRatio = clampCast<float>(j->numberAt("coarseRatio", o.coarseRatio));
    o.fineCents = clampCast<float>(j->numberAt("fineCents", o.fineCents));
    o.fixedHz = clampCast<float>(j->numberAt("fixedHz", o.fixedHz));
    o.detuneCents = clampCast<float>(j->numberAt("detuneCents", o.detuneCents));
    o.outputLevel = clampCast<uint8_t>(j->numberAt("outputLevel", o.outputLevel));
    o.ampEG = egParamsFromJson(j->find("ampEG"));
    o.fxEG = egParamsFromJson(j->find("fxEG"));
    o.levelScaling = keyLevelScalingFromJson(j->find("levelScaling"));
    o.rateScaling = clampCast<uint8_t>(j->numberAt("rateScaling", o.rateScaling));
    o.velocitySens = clampCast<uint8_t>(j->numberAt("velocitySens", o.velocitySens));
    o.lfoAmpModSens = clampCast<uint8_t>(j->numberAt("lfoAmpModSens", o.lfoAmpModSens));
    o.lfoPitchModSens = clampCast<uint8_t>(j->numberAt("lfoPitchModSens", o.lfoPitchModSens));
    o.feedback = clampCast<float>(j->numberAt("feedback", o.feedback));
    o.waveform = waveformFromName(j->stringAt("waveform", waveformName(o.waveform)));
    o.phaseDistortion = clampCast<float>(j->numberAt("phaseDistortion", o.phaseDistortion));
    o.fxType = fxTypeFromName(j->stringAt("fxType", fxTypeName(o.fxType)));
    o.fxParam1 = clampCast<float>(j->numberAt("fxParam1", o.fxParam1));
    o.fxParam2 = clampCast<float>(j->numberAt("fxParam2", o.fxParam2));
    o.fxKeyTrack = clampCast<float>(j->numberAt("fxKeyTrack", o.fxKeyTrack)); // ausente (patch antigo) = 0
    o.ringModSource = clampCast<int>(j->numberAt("ringModSource", o.ringModSource));
    o.fxEGReleaseWithAmp = j->boolAt("fxEGReleaseWithAmp", o.fxEGReleaseWithAmp);
    o.pegDepth = clampCast<float>(j->numberAt("pegDepth", o.pegDepth));
    o.pegAffectsFixed = j->boolAt("pegAffectsFixed", o.pegAffectsFixed);
    if (auto* add = j->find("additive")) { // ausente (patch antigo) = mantem os defaults
        o.additiveA = partialSetFromJson(add->find("a"));
        o.additiveB = partialSetFromJson(add->find("b"));
        o.morphDepth = clampCast<float>(add->numberAt("morph", o.morphDepth));
    }
    return o;
}

inline JsonValue modRouteToJson(const ModRoute& r) {
    JsonValue v = JsonValue::makeObject();
    v.set("src", JsonValue::makeString(modSourceName(r.src)));
    v.set("dest", JsonValue::makeString(modDestName(r.dest)));
    v.set("opIndex", JsonValue::makeNumber(r.opIndex));
    v.set("depth", JsonValue::makeNumber(r.depth));
    return v;
}
inline ModRoute modRouteFromJson(const JsonValue* j) {
    ModRoute r;
    if (!j) return r;
    r.src = modSourceFromName(j->stringAt("src", modSourceName(r.src)));
    r.dest = modDestFromName(j->stringAt("dest", modDestName(r.dest)));
    r.opIndex = clampCast<int>(j->numberAt("opIndex", r.opIndex));
    r.depth = clampCast<float>(j->numberAt("depth", r.depth));
    return r;
}

inline JsonValue algorithmMatrixToJson(const AlgorithmMatrix& m) {
    JsonValue v = JsonValue::makeObject();
    JsonValue mod = JsonValue::makeArray();
    JsonValue ring = JsonValue::makeArray();
    JsonValue outMix = JsonValue::makeArray();
    for (int i = 0; i < kNumOps; ++i) {
        JsonValue modRow = JsonValue::makeArray();
        JsonValue ringRow = JsonValue::makeArray();
        for (int k = 0; k < kNumOps; ++k) {
            modRow.push(JsonValue::makeNumber(m.mod[i][k]));
            ringRow.push(JsonValue::makeBool(m.ring[i][k]));
        }
        mod.push(std::move(modRow));
        ring.push(std::move(ringRow));
        outMix.push(JsonValue::makeNumber(m.outputMix[i]));
    }
    v.set("mod", std::move(mod));
    v.set("ring", std::move(ring));
    v.set("outputMix", std::move(outMix));
    return v;
}
inline AlgorithmMatrix algorithmMatrixFromJson(const JsonValue* j) {
    AlgorithmMatrix m{};
    if (!j) return m;
    if (auto* mod = j->find("mod"))
        for (int i = 0; i < kNumOps && i < static_cast<int>(mod->arraySize()); ++i) {
            const JsonValue& row = mod->at(static_cast<size_t>(i));
            for (int k = 0; k < kNumOps && k < static_cast<int>(row.arraySize()); ++k)
                m.mod[i][k] = clampCast<float>(row.at(static_cast<size_t>(k)).asNumber(0.0));
        }
    if (auto* ring = j->find("ring"))
        for (int i = 0; i < kNumOps && i < static_cast<int>(ring->arraySize()); ++i) {
            const JsonValue& row = ring->at(static_cast<size_t>(i));
            for (int k = 0; k < kNumOps && k < static_cast<int>(row.arraySize()); ++k)
                m.ring[i][k] = row.at(static_cast<size_t>(k)).asBool(false);
        }
    if (auto* outMix = j->find("outputMix"))
        for (int i = 0; i < kNumOps && i < static_cast<int>(outMix->arraySize()); ++i)
            m.outputMix[i] = clampCast<float>(outMix->at(static_cast<size_t>(i)).asNumber(0.0));
    return m;
}

inline JsonValue voiceParamsToJson(const VoiceParams& v) {
    JsonValue j = JsonValue::makeObject();
    JsonValue ops = JsonValue::makeArray();
    for (int i = 0; i < kNumOps; ++i) ops.push(operatorParamsToJson(v.op[i]));
    j.set("op", std::move(ops));
    j.set("matrix", algorithmMatrixToJson(v.matrix));
    j.set("peg", pegParamsToJson(v.peg));
    j.set("lfo1", lfoParamsToJson(v.lfo1));
    j.set("lfo2", lfoParamsToJson(v.lfo2));
    JsonValue mm = JsonValue::makeArray();
    for (int i = 0; i < kNumModRoutes; ++i) mm.push(modRouteToJson(v.modMatrix[i]));
    j.set("modMatrix", std::move(mm));
    return j;
}
inline VoiceParams voiceParamsFromJson(const JsonValue& j) {
    VoiceParams v; // ja vem com o patch minimo default (op0 carrier, op1 modula op0)
    if (j.type() != JsonValue::Type::Object) return v;
    if (auto* ops = j.find("op"))
        for (int i = 0; i < kNumOps && i < static_cast<int>(ops->arraySize()); ++i)
            v.op[i] = operatorParamsFromJson(&ops->at(static_cast<size_t>(i)));
    // So sobrescreve matrix/peg/lfo1/lfo2 se a chave existir -- senao, um JSON
    // incompleto (de uma versao antiga, por ex.) apagaria os defaults acima
    // com uma AlgorithmMatrix vazia em vez de preserva-los.
    if (auto* matrix = j.find("matrix")) v.matrix = algorithmMatrixFromJson(matrix);
    if (auto* peg = j.find("peg")) v.peg = pegParamsFromJson(peg);
    if (auto* lfo1 = j.find("lfo1")) v.lfo1 = lfoParamsFromJson(lfo1);
    if (auto* lfo2 = j.find("lfo2")) v.lfo2 = lfoParamsFromJson(lfo2);
    if (auto* mm = j.find("modMatrix"))
        for (int i = 0; i < kNumModRoutes && i < static_cast<int>(mm->arraySize()); ++i)
            v.modMatrix[i] = modRouteFromJson(&mm->at(static_cast<size_t>(i)));
    sanitize(v);
    return v;
}

inline JsonValue partParamsToJson(const PartParams& p) {
    JsonValue j = JsonValue::makeObject();
    j.set("name", JsonValue::makeString(p.name));
    j.set("voice", voiceParamsToJson(p.voice));
    j.set("midiChannel", JsonValue::makeNumber(p.midiChannel));
    j.set("loKey", JsonValue::makeNumber(p.loKey));
    j.set("hiKey", JsonValue::makeNumber(p.hiKey));
    j.set("loVel", JsonValue::makeNumber(p.loVel));
    j.set("hiVel", JsonValue::makeNumber(p.hiVel));
    j.set("transpose", JsonValue::makeNumber(p.transpose));
    j.set("level", JsonValue::makeNumber(p.level));
    j.set("pan", JsonValue::makeNumber(p.pan));
    j.set("voiceMode", JsonValue::makeString(voiceModeName(p.voiceMode)));
    j.set("unisonCount", JsonValue::makeNumber(p.unisonCount));
    j.set("unisonDetuneCents", JsonValue::makeNumber(p.unisonDetuneCents));
    j.set("unisonSpread", JsonValue::makeNumber(p.unisonSpread));
    j.set("glideEnabled", JsonValue::makeBool(p.glideEnabled));
    j.set("glideTimeSec", JsonValue::makeNumber(p.glideTimeSec));
    j.set("glideLegatoOnly", JsonValue::makeBool(p.glideLegatoOnly));
    j.set("monoLegatoEnvReuse", JsonValue::makeBool(p.monoLegatoEnvReuse));
    j.set("reservedVoices", JsonValue::makeNumber(p.reservedVoices));
    j.set("enabled", JsonValue::makeBool(p.enabled));
    j.set("bendRangeSemis", JsonValue::makeNumber(p.bendRangeSemis));
    return j;
}
inline PartParams partParamsFromJson(const JsonValue& j) {
    PartParams p;
    p.name = j.stringAt("name", p.name);
    if (auto* voice = j.find("voice")) p.voice = voiceParamsFromJson(*voice);
    p.midiChannel = clampCast<int>(j.numberAt("midiChannel", p.midiChannel));
    p.loKey = clampCast<uint8_t>(j.numberAt("loKey", p.loKey));
    p.hiKey = clampCast<uint8_t>(j.numberAt("hiKey", p.hiKey));
    p.loVel = clampCast<uint8_t>(j.numberAt("loVel", p.loVel));
    p.hiVel = clampCast<uint8_t>(j.numberAt("hiVel", p.hiVel));
    p.transpose = clampCast<int8_t>(j.numberAt("transpose", p.transpose));
    p.level = clampCast<float>(j.numberAt("level", p.level));
    p.pan = clampCast<float>(j.numberAt("pan", p.pan));
    p.voiceMode = voiceModeFromName(j.stringAt("voiceMode", voiceModeName(p.voiceMode)));
    p.unisonCount = clampCast<int>(j.numberAt("unisonCount", p.unisonCount));
    p.unisonDetuneCents = clampCast<float>(j.numberAt("unisonDetuneCents", p.unisonDetuneCents));
    p.unisonSpread = clampCast<float>(j.numberAt("unisonSpread", p.unisonSpread));
    p.glideEnabled = j.boolAt("glideEnabled", p.glideEnabled);
    p.glideTimeSec = clampCast<float>(j.numberAt("glideTimeSec", p.glideTimeSec));
    p.glideLegatoOnly = j.boolAt("glideLegatoOnly", p.glideLegatoOnly);
    p.monoLegatoEnvReuse = j.boolAt("monoLegatoEnvReuse", p.monoLegatoEnvReuse);
    p.reservedVoices = clampCast<int>(j.numberAt("reservedVoices", p.reservedVoices));
    p.enabled = j.boolAt("enabled", p.enabled);
    p.bendRangeSemis = clampCast<float>(j.numberAt("bendRangeSemis", p.bendRangeSemis));
    sanitize(p);
    return p;
}

// --- I/O de arquivo ------------------------------------------------------

inline bool savePatchFile(const std::string& path, const VoiceParams& v) {
    std::ofstream f(path);
    if (!f) return false;
    f << voiceParamsToJson(v).dump(0);
    return f.good();
}
inline bool loadPatchFile(const std::string& path, VoiceParams& v) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    JsonValue j = JsonValue::parse(ss.str());
    if (j.type() != JsonValue::Type::Object) return false; // vazio/lixo: nao troca o patch em silencio
    v = voiceParamsFromJson(j);
    return true;
}

inline bool savePartFile(const std::string& path, const PartParams& p) {
    std::ofstream f(path);
    if (!f) return false;
    f << partParamsToJson(p).dump(0);
    return f.good();
}
inline bool loadPartFile(const std::string& path, PartParams& p) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    JsonValue j = JsonValue::parse(ss.str());
    if (j.type() != JsonValue::Type::Object) return false;
    p = partParamsFromJson(j);
    return true;
}

} // namespace dx7fm
