#pragma once
#include "dx7_params.h"
#include "dx7_engine.h"
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dx7fm {

constexpr int kNumParts = 4;

struct VoiceSlot {
    Voice voice;
    int ownerPart = -1;       // -1 = livre
    int note = -1;            // nota MIDI FISICA (sem transpose)
    bool physicallyHeld = false;
    bool sustainHeld = false;
    float subPan = 0.0f;
    uint64_t triggerCounter = 0;
};

struct PartRuntime {
    PartParams params;
    CompiledAlgorithm compiled;
    std::vector<int> heldStack;             // notas fisicas pressionadas, ordem de chegada
    std::vector<int> activeSlots;           // slots do grupo mono/unison atual
    std::unordered_map<int, int> noteToSlot;// so usado no modo Poly
    std::unordered_set<int> sostenutoCaptured;
    bool sostenutoDown = false;
    bool sustainDown = false;
    float bend = 0.0f;
    float modWheel = 0.0f;
    float channelAT = 0.0f;
};

class Synth {
public:
    // oversample: 1 (default, sem custo extra), 2 ou 4 -- ver Voice::setOversample()
    // e ARQUITETURA.txt secao 11 (oversampling anti-aliasing).
    void init(double sampleRate, int maxVoices = 64, int oversample = 1) {
        sampleRate_ = sampleRate;
        slots_.assign(static_cast<size_t>(maxVoices), VoiceSlot{});
        for (auto& s : slots_) {
            s.voice.setOversample(oversample);
            s.voice.setSampleRate(sampleRate);
        }
        for (int p = 0; p < kNumParts; ++p) recompile(p);
    }

    PartParams& part(int idx) { return parts_[idx].params; }

    void recompile(int idx) {
        parts_[idx].compiled = compileAlgorithm(parts_[idx].params.voice.matrix);
    }

    void noteOn(int part, int note, int velocity) {
        auto& pr = parts_[part];
        if (!pr.params.enabled) return;
        if (note < pr.params.loKey || note > pr.params.hiKey) return;
        if (velocity < pr.params.loVel || velocity > pr.params.hiVel) return;

        switch (pr.params.voiceMode) {
            case VoiceMode::Poly:   polyNoteOn(part, note, velocity); break;
            case VoiceMode::Mono:   monoGroupNoteOn(part, note, velocity, 1); break;
            case VoiceMode::Unison: monoGroupNoteOn(part, note, velocity, std::max(1, pr.params.unisonCount)); break;
        }
    }

    void noteOff(int part, int note) {
        auto& pr = parts_[part];
        switch (pr.params.voiceMode) {
            case VoiceMode::Poly: polyNoteOff(part, note); break;
            case VoiceMode::Mono:
            case VoiceMode::Unison: monoGroupNoteOff(part, note); break;
        }
    }

    void polyAftertouch(int part, int note, float pressure01) {
        auto& pr = parts_[part];
        if (pr.params.voiceMode == VoiceMode::Poly) {
            auto it = pr.noteToSlot.find(note);
            if (it != pr.noteToSlot.end()) slots_[it->second].voice.controllers().polyAT01 = pressure01;
        } else if (!pr.heldStack.empty() && pr.heldStack.back() == note) {
            for (int idx : pr.activeSlots) slots_[idx].voice.controllers().polyAT01 = pressure01;
        }
    }

    void channelAftertouch(int part, float pressure01) {
        auto& pr = parts_[part];
        pr.channelAT = pressure01;
        for (auto& s : slots_) if (s.ownerPart == part) s.voice.controllers().channelAT01 = pressure01;
    }

    void pitchBend(int part, float semis) {
        auto& pr = parts_[part];
        pr.bend = semis;
        for (auto& s : slots_) if (s.ownerPart == part) s.voice.controllers().pitchBendSemis = semis;
    }

    void modWheel(int part, float v01) {
        auto& pr = parts_[part];
        pr.modWheel = v01;
        for (auto& s : slots_) if (s.ownerPart == part) s.voice.controllers().modWheel01 = v01;
    }

    void sustainPedal(int part, bool down) {
        auto& pr = parts_[part];
        pr.sustainDown = down;
        if (!down) {
            for (auto& s : slots_) {
                if (s.ownerPart == part && s.sustainHeld && !s.physicallyHeld) {
                    s.sustainHeld = false;
                    s.voice.noteOff();
                }
            }
        }
    }

    void sostenutoPedal(int part, bool down) {
        auto& pr = parts_[part];
        pr.sostenutoDown = down;
        if (down) {
            pr.sostenutoCaptured.clear();
            for (int n : pr.heldStack) pr.sostenutoCaptured.insert(n);
        } else {
            pr.sostenutoCaptured.clear();
        }
    }

    void allNotesOff(int part) {
        auto& pr = parts_[part];
        for (auto& s : slots_) {
            if (s.ownerPart == part) {
                s.voice.noteOff();
                s.physicallyHeld = false;
                s.sustainHeld = false;
            }
        }
        pr.heldStack.clear();
        pr.activeSlots.clear();
        pr.noteToSlot.clear();
        pr.sostenutoCaptured.clear();
    }

    void renderStereo(float& outL, float& outR) {
        outL = 0.0f;
        outR = 0.0f;
        for (auto& s : slots_) {
            if (s.ownerPart < 0 || !s.voice.isActive()) continue;
            auto& pr = parts_[s.ownerPart];
            float sample = s.voice.process();
            float pan = std::clamp(pr.params.pan + s.subPan, -1.0f, 1.0f);
            float gL = 1.0f - std::max(0.0f, pan);
            float gR = 1.0f + std::min(0.0f, pan);
            float lvl = pr.params.level * sample;
            outL += lvl * gL;
            outR += lvl * gR;
        }
    }

    int activeVoiceCount() const {
        int c = 0;
        for (auto& s : slots_) if (s.ownerPart >= 0 && s.voice.isActive()) ++c;
        return c;
    }

    int activeVoiceCount(int part) const {
        int c = 0;
        for (auto& s : slots_) if (s.ownerPart == part && s.voice.isActive()) ++c;
        return c;
    }

    Voice& debugVoiceOfPart(int part, int k = 0) {
        int c = 0;
        for (auto& s : slots_) {
            if (s.ownerPart == part && s.voice.isActive()) {
                if (c == k) return s.voice;
                ++c;
            }
        }
        return slots_[0].voice;
    }

private:
    int countActive(int part) const { return activeVoiceCount(part); }

    int allocateSlot(int requestingPart) {
        for (size_t i = 0; i < slots_.size(); ++i)
            if (!slots_[i].voice.isActive()) return static_cast<int>(i);

        int best = -1;
        for (size_t i = 0; i < slots_.size(); ++i) {
            auto& s = slots_[i];
            if (s.ownerPart != requestingPart && s.ownerPart >= 0) {
                if (countActive(s.ownerPart) <= parts_[s.ownerPart].params.reservedVoices) continue;
            }
            if (best < 0) { best = static_cast<int>(i); continue; }
            auto& sb = slots_[static_cast<size_t>(best)];
            bool sRel = s.voice.isReleasing();
            bool bRel = sb.voice.isReleasing();
            if (sRel != bRel) {
                if (sRel) best = static_cast<int>(i);
                continue;
            }
            if (s.triggerCounter < sb.triggerCounter) best = static_cast<int>(i);
        }
        return best < 0 ? 0 : best;
    }

    void configureVoice(Voice& v, const PartRuntime& pr, bool glideActiveForThisNote) {
        v.glideEnabled_ = pr.params.glideEnabled && (glideActiveForThisNote || !pr.params.glideLegatoOnly);
        v.glideTimeSec_ = pr.params.glideTimeSec;
        v.reuseEnvOnLegato_ = pr.params.monoLegatoEnvReuse;
        v.bendRangeSemis_ = pr.params.bendRangeSemis;
    }

    void triggerSlot(int idx, int part, int note, int velocity, float extraDetuneCents, bool legato) {
        auto& pr = parts_[part];
        auto& s = slots_[static_cast<size_t>(idx)];
        configureVoice(s.voice, pr, legato);
        int soundingNote = std::clamp(note + pr.params.transpose, 0, 127);
        s.ownerPart = part;
        s.note = note;
        s.physicallyHeld = true;
        s.sustainHeld = false;
        s.triggerCounter = ++triggerCounter_;
        s.voice.noteOn(&pr.params.voice, &pr.compiled, soundingNote, velocity, extraDetuneCents, legato);
        s.voice.controllers().pitchBendSemis = pr.bend;
        s.voice.controllers().modWheel01 = pr.modWheel;
        s.voice.controllers().channelAT01 = pr.channelAT;
    }

    void releaseSlot(int idx, int part, int note) {
        auto& pr = parts_[part];
        auto& s = slots_[static_cast<size_t>(idx)];
        s.physicallyHeld = false;
        bool held = pr.sustainDown || (pr.sostenutoDown && pr.sostenutoCaptured.count(note) > 0);
        if (held) s.sustainHeld = true;
        else s.voice.noteOff();
    }

    void polyNoteOn(int part, int note, int velocity) {
        auto& pr = parts_[part];
        auto it = pr.noteToSlot.find(note);
        bool retrigger = (it != pr.noteToSlot.end());
        int idx = retrigger ? it->second : allocateSlot(part);
        pr.noteToSlot[note] = idx;
        triggerSlot(idx, part, note, velocity, 0.0f, false);
        pr.heldStack.push_back(note);
    }

    void polyNoteOff(int part, int note) {
        auto& pr = parts_[part];
        auto it = pr.noteToSlot.find(note);
        if (it == pr.noteToSlot.end()) return;
        int idx = it->second;
        pr.noteToSlot.erase(it);
        auto& hs = pr.heldStack;
        hs.erase(std::remove(hs.begin(), hs.end(), note), hs.end());
        releaseSlot(idx, part, note);
    }

    // Usado tanto por Mono (voiceCount=1) quanto Unison (voiceCount=unisonCount).
    void monoGroupNoteOn(int part, int note, int velocity, int voiceCount) {
        auto& pr = parts_[part];
        bool legato = !pr.activeSlots.empty();
        pr.heldStack.push_back(note);
        if (legato) {
            monoGroupNoteOnInternal(part, note, velocity, voiceCount);
            return;
        }
        pr.activeSlots.clear();
        for (int k = 0; k < voiceCount; ++k) {
            int idx = allocateSlot(part);
            float t = voiceCount > 1 ? ((static_cast<float>(k) / static_cast<float>(voiceCount - 1)) - 0.5f) * 2.0f : 0.0f;
            float detune = t * pr.params.unisonDetuneCents;
            triggerSlot(idx, part, note, velocity, detune, false);
            slots_[static_cast<size_t>(idx)].subPan = t * pr.params.unisonSpread;
            pr.activeSlots.push_back(idx);
        }
    }

    // Variante interna usada so pelo reataque legato: ja sabe que legato=true,
    // nao reavalia a decisao de alocacao (evita duplicar logica).
    void monoGroupNoteOnInternal(int part, int note, int velocity, int voiceCount) {
        auto& pr = parts_[part];
        for (size_t k = 0; k < pr.activeSlots.size(); ++k) {
            int idx = pr.activeSlots[k];
            float t = voiceCount > 1 ? ((static_cast<float>(k) / static_cast<float>(voiceCount - 1)) - 0.5f) * 2.0f : 0.0f;
            float detune = t * pr.params.unisonDetuneCents;
            // LIMITACAO CONHECIDA: velocity no reataque mono e sempre a informada
            // aqui (fixa em 100 quando vem de monoGroupNoteOff), pois heldStack
            // so guarda a nota, nao o par (nota, velocity).
            triggerSlot(idx, part, note, velocity, detune, true);
        }
    }

    void monoGroupNoteOff(int part, int note) {
        auto& pr = parts_[part];
        auto& hs = pr.heldStack;
        hs.erase(std::remove(hs.begin(), hs.end(), note), hs.end());
        if (!hs.empty()) {
            int prevNote = hs.back();
            int voiceCount = static_cast<int>(pr.activeSlots.size());
            monoGroupNoteOnInternal(part, prevNote, 100, voiceCount);
        } else {
            for (int idx : pr.activeSlots) releaseSlot(idx, part, note);
            pr.activeSlots.clear();
        }
    }

    double sampleRate_ = 48000.0;
    std::vector<VoiceSlot> slots_;
    PartRuntime parts_[kNumParts];
    uint64_t triggerCounter_ = 0;
};

} // namespace dx7fm
