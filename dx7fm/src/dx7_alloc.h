#pragma once
#include "dx7_params.h"
#include "dx7_engine.h"
#include "dx7_globalfx.h"
#include <algorithm>
#include <array>
#include <atomic>
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
    // params.voice e a copia de STAGING: Synth::part(idx) devolve referencia
    // pra ela, entao editar synth.part(idx).voice.X so afeta o audio depois
    // que Synth::recompile(idx) publicar um PatchSnapshot novo (ver abaixo).
    // Os demais campos de PartParams (transpose, loKey/hiKey, voiceMode etc.)
    // continuam single-buffer, sem essa indirecao -- nao sao "o timbre" que
    // o item de thread-safety do roteiro (secao 11) pedia pra proteger.
    PartParams params;

    // [ADICIONADO em 2026-09-21] Triple buffer + indice atomico: mecanismo
    // de troca atomica de patch (o item "thread safety" do roteiro). O
    // audio thread SO le snapshots_[liveIndex_] (via live()), nunca o slot
    // de staging, entao editar params.voice de outra thread nao e uma data
    // race com o audio -- desde que quem edita va SEMPRE por
    // Synth::part(idx) + Synth::recompile(idx), nunca escrevendo direto
    // num snapshot. 3 slots (nao 2) pra nunca reescrever o slot que ACABOU
    // de ser live (poderia ainda estar em leitura numa renderStereo() em
    // andamento na outra thread).
    std::array<PatchSnapshot, 3> snapshots_{};
    std::atomic<int> liveIndex_{0};
    int stagingIndex_ = 1;

    const PatchSnapshot* live() const {
        return &snapshots_[static_cast<size_t>(liveIndex_.load(std::memory_order_acquire))];
    }

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
        chorus_.setSampleRate(sampleRate);
        delay_.setSampleRate(sampleRate);
        reverb_.setSampleRate(sampleRate);
    }

    // Referencia de STAGING: editar campos aqui (inclusive .voice, o timbre)
    // so afeta a reproducao apos o proximo recompile(idx). Seguro de chamar
    // de uma thread de UI/edicao diferente da que roda renderStereo(), desde
    // que a UNICA forma de "publicar" a edicao seja via recompile(idx).
    PartParams& part(int idx) { return parts_[idx].params; }

    // [ADICIONADO em 2026-09-21] Efeitos globais POS-MIX (chorus, delay,
    // reverb -- ver dx7_globalfx.h e dx7_params.h::GlobalFxParams).
    // Aplicados uma vez na SOMA das 4 partes, dentro de renderStereo().
    // Acesso direto (nao passa pelo triple-buffer do timbre, ver secao 7C
    // do ARQUITETURA.txt sobre o que NAO esta coberto pela garantia de
    // thread safety) -- editar globalFx() enquanto renderStereo() roda
    // noutra thread tem a mesma ressalva que os demais campos "fora do
    // timbre" de PartParams.
    GlobalFxParams& globalFx() { return globalFx_; }

    // Publica params.voice (staging) como o novo patch AO VIVO: recompila o
    // algoritmo, escreve o par (voice, compiled) casado num slot de buffer
    // que o audio thread NAO esta lendo, e so entao troca o indice atomico
    // -- ver PartRuntime acima. OBRIGATORIO apos qualquer edicao de
    // part(idx).voice (matriz OU qualquer outro parametro do timbre), nao
    // so mudancas de topologia. Vozes JA soando veem o novo patch no proximo
    // renderStereo() (Synth::rebind ao vivo), preservando a edicao "ao vivo"
    // (ver ARQUITETURA.txt secao 10) sem data race.
    void recompile(int idx) {
        auto& pr = parts_[idx];
        int newIdx = pr.stagingIndex_;
        pr.snapshots_[static_cast<size_t>(newIdx)] = makeSnapshot(pr.params.voice);
        int oldLive = pr.liveIndex_.exchange(newIdx, std::memory_order_acq_rel);
        pr.stagingIndex_ = 3 - newIdx - oldLive; // o terceiro indice (0+1+2=3)
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

        // 1 load atomico (acquire) por parte, nao por voz -- barato mesmo
        // com muitas vozes ativas da mesma parte.
        const PatchSnapshot* live[kNumParts];
        for (int p = 0; p < kNumParts; ++p) live[p] = parts_[p].live();

        for (auto& s : slots_) {
            if (s.ownerPart < 0 || !s.voice.isActive()) continue;
            auto& pr = parts_[s.ownerPart];
            const PatchSnapshot* snap = live[s.ownerPart];
            s.voice.rebind(&snap->voice, &snap->compiled); // "ao vivo": pega qualquer recompile() publicado ate agora
            float sample = s.voice.process();
            float pan = std::clamp(pr.params.pan + s.subPan, -1.0f, 1.0f);
            float gL = 1.0f - std::max(0.0f, pan);
            float gR = 1.0f + std::min(0.0f, pan);
            float lvl = pr.params.level * sample;
            outL += lvl * gL;
            outR += lvl * gR;
        }

        // Efeitos globais pos-mix, na soma das 4 partes: chorus -> delay ->
        // reverb (cadeia fixa; cada estagio e transparente se !enabled).
        float chL, chR, dlL, dlR;
        chorus_.process(globalFx_.chorus, outL, outR, chL, chR);
        delay_.process(globalFx_.delay, chL, chR, dlL, dlR);
        reverb_.process(globalFx_.reverb, dlL, dlR, outL, outR);
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
        const PatchSnapshot* snap = pr.live(); // notas NOVAS sempre pegam o patch AO VIVO mais recente
        s.voice.noteOn(&snap->voice, &snap->compiled, soundingNote, velocity, extraDetuneCents, legato);
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

    GlobalFxParams globalFx_;
    StereoChorus chorus_;
    StereoDelay delay_;
    SimpleReverb reverb_;
};

} // namespace dx7fm
