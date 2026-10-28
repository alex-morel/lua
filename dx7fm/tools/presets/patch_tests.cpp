// [ADICIONADO em 2026-10-28] Testa os patches de tools/presets/gen_presets.cpp:
// o comportamento prometido por cada um, faixa do teclado/velocity, acorde de
// 8 notas, custo de CPU. Grava os cenarios em .wav; os espectrogramas saem de
// tools/presets/spectrogram.py. Uso, na pasta dx7fm/:
//   ./build/patch_tests [pasta_dos_patches] [pasta_dos_wav]
#include "dx7_alloc.h"
#include "dx7_fft.h"
#include "dx7_patch_io.h"
#include "wav_writer.h"
#include "patch_browser.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
using namespace dx7fm;
namespace fs = std::filesystem;

static std::string g_dir, g_out;
static int g_fail = 0, g_pass = 0;
static void expect(bool ok, const std::string& patch, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FALHOU", what.c_str());
    (ok ? g_pass : g_fail)++;
    (void)patch;
}

struct Ev { double t; int note; int vel; bool on; };
struct Take { std::vector<float> l, r; };

// toca uma sequencia de eventos num synth com o patch; extra(synth) antes de comecar
static Take play(const PartParams& p, const std::vector<Ev>& evs, double seconds, float bpm = 120.0f,
                 const std::function<void(Synth&)>& extra = nullptr, const std::function<void(Synth&, double)>& during = nullptr) {
    Synth s;
    s.init(48000.0, 64);
    s.setTempoBpm(bpm);
    s.part(0) = p;
    s.recompile(0);
    for (int k = 0; k < VoiceParams::kNumMacros; ++k) s.setMacro(0, k, p.voice.macro[k]);
    if (extra) extra(s);
    Take t;
    const int n = static_cast<int>(seconds * 48000);
    t.l.resize(static_cast<size_t>(n));
    t.r.resize(static_cast<size_t>(n));
    size_t e = 0;
    for (int i = 0; i < n; i += 48) { // eventos com resolucao de 1 ms
        const double now = i / 48000.0;
        while (e < evs.size() && evs[e].t <= now) {
            if (evs[e].on) s.noteOn(0, evs[e].note, evs[e].vel); else s.noteOff(0, evs[e].note);
            ++e;
        }
        if (during) during(s, now);
        s.renderBlock(&t.l[static_cast<size_t>(i)], &t.r[static_cast<size_t>(i)], std::min(48, n - i));
    }
    return t;
}
static std::vector<Ev> chord(std::initializer_list<int> notes, double on, double off, int vel = 100) {
    std::vector<Ev> v;
    for (int n : notes) v.push_back({on, n, vel, true});
    for (int n : notes) v.push_back({off, n, vel, false});
    std::sort(v.begin(), v.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    return v;
}

// --- medidas -----------------------------------------------------------------------
static std::vector<float> seg(const std::vector<float>& x, double from, double len) {
    size_t a = static_cast<size_t>(std::max(0.0, from) * 48000), b = std::min(x.size(), a + static_cast<size_t>(len * 48000));
    return std::vector<float>(x.begin() + static_cast<long>(a), x.begin() + static_cast<long>(b));
}
static float rms(const std::vector<float>& x) {
    double s = 0; for (float v : x) s += static_cast<double>(v) * v;
    return x.empty() ? 0.0f : static_cast<float>(std::sqrt(s / x.size()));
}
// espectro de magnitude (Hann, n potencia de 2)
static std::vector<float> spectrum(const std::vector<float>& x, int n = 8192) {
    std::vector<std::complex<float>> b(static_cast<size_t>(n));
    for (int i = 0; i < n && i < static_cast<int>(x.size()); ++i)
        b[static_cast<size_t>(i)] = x[static_cast<size_t>(i)] * (0.5f - 0.5f * std::cos(6.2831853f * i / n));
    fftInPlace(b);
    std::vector<float> m(static_cast<size_t>(n / 2));
    for (int k = 0; k < n / 2; ++k) m[static_cast<size_t>(k)] = std::abs(b[static_cast<size_t>(k)]);
    return m;
}
static float bandEnergy(const std::vector<float>& x, double f0, double f1);
// brilho: fracao da energia acima de 2.5 x f0 (o centroide ficava preso na fundamental)
static float bright(const std::vector<float>& x, float f0) { return bandEnergy(x, 2.5 * f0, 18000) / (bandEnergy(x, 20, 18000) + 1e-12f); }
static float bandEnergy(const std::vector<float>& x, double f0, double f1) {
    auto m = spectrum(x);
    double s = 0;
    for (size_t k = static_cast<size_t>(f0 / 48000.0 * 8192); k <= static_cast<size_t>(f1 / 48000.0 * 8192) && k < m.size(); ++k) s += m[k] * m[k];
    return static_cast<float>(s);
}
// altura por autocorrelacao normalizada (40..2000 Hz), com interpolacao parabolica
static float pitchHz(const std::vector<float>& x) {
    const int n = std::min<int>(static_cast<int>(x.size()), 4096);
    const int minLag = 24, maxLag = 1200;
    std::vector<double> r(static_cast<size_t>(maxLag + 2), 0.0);
    for (int lag = minLag - 1; lag <= maxLag + 1; ++lag) {
        double s = 0, e0 = 0, e1 = 0;
        for (int i = 0; i + lag < n; ++i) { s += x[static_cast<size_t>(i)] * x[static_cast<size_t>(i + lag)]; e0 += x[static_cast<size_t>(i)] * x[static_cast<size_t>(i)]; e1 += x[static_cast<size_t>(i + lag)] * x[static_cast<size_t>(i + lag)]; }
        r[static_cast<size_t>(lag)] = s / std::sqrt(e0 * e1 + 1e-20);
    }
    // primeiro pico "bom" (>= 90% do maximo): evita pular pra oitava abaixo
    double best = 0; for (int lag = minLag; lag <= maxLag; ++lag) best = std::max(best, r[static_cast<size_t>(lag)]);
    for (int lag = minLag; lag <= maxLag; ++lag) {
        const double a = r[static_cast<size_t>(lag - 1)], b = r[static_cast<size_t>(lag)], c = r[static_cast<size_t>(lag + 1)];
        if (b >= a && b >= c && b >= 0.9 * best) {
            double d = a - 2 * b + c, p = d < 0 ? 0.5 * (a - c) / d : 0.0;
            return static_cast<float>(48000.0 / (lag + p));
        }
    }
    return 0.0f;
}
static float goertzel(const std::vector<float>& x, double f) {
    double w = 2 * M_PI * f / 48000.0, c = 2 * std::cos(w), s1 = 0, s2 = 0;
    for (size_t i = 0; i < x.size(); ++i) { double win = 0.5 - 0.5 * std::cos(2 * M_PI * i / x.size()); double s0 = x[i] * win + c * s1 - s2; s2 = s1; s1 = s0; }
    return static_cast<float>(std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2));
}
static float pitchNear(const std::vector<float>& x, float ref) { // pico entre -1 e +1 semitom, passo de 1 cent
    float best = 0, bestF = ref;
    for (int c = -100; c <= 100; ++c) { float f = ref * std::pow(2.0f, c / 1200.0f), m = goertzel(x, f); if (m > best) { best = m; bestF = f; } }
    return bestF;
}
static float zcHz(const std::vector<float>& x) { // periodo medio entre cruzamentos subindo (sinal quase senoidal)
    std::vector<double> z;
    for (size_t i = 1; i < x.size(); ++i) if (x[i - 1] < 0 && x[i] >= 0) z.push_back(i - 1 + x[i - 1] / (x[i - 1] - x[i]));
    return z.size() < 2 ? 0.0f : static_cast<float>(48000.0 * (z.size() - 1) / (z.back() - z.front()));
}
static float semis(float hz, float ref) { return hz > 0 && ref > 0 ? 12.0f * std::log2(hz / ref) : 99.0f; }
static float noteHz(int n) { return Voice::noteToHz(n); }
// inicios de nota: subidas da envoltoria RMS (janelas de 2 ms)
static std::vector<double> onsets(const std::vector<float>& x, double thresholdRatio = 2.0) {
    std::vector<float> env;
    for (size_t i = 0; i + 96 <= x.size(); i += 96) env.push_back(rms(std::vector<float>(x.begin() + static_cast<long>(i), x.begin() + static_cast<long>(i + 96))));
    std::vector<double> out;
    for (size_t i = 3; i < env.size(); ++i)
        if (env[i] > 0.01f && env[i] > thresholdRatio * env[i - 3] && (out.empty() || i * 0.002 - out.back() > 0.05)) out.push_back(i * 0.002);
    return out;
}
static void save(const std::string& name, const Take& t) { writeWavStereo(g_out + "/" + name + ".wav", t.l, t.r, 48000); }

// --- testes gerais (todo patch) ----------------------------------------------------
static void general(const std::string& name, const PartParams& p) {
    bool finite = true, ok = true;
    float worstPeak = 0, quietest = 1;
    std::string bad;
    for (int note = 24; note <= 108; note += 12)
        for (int vel : {30, 127}) {
            auto t = play(p, chord({note}, 0.0, 1.5, vel), 2.0);
            float pk = 0; for (size_t i = 0; i < t.l.size(); ++i) { pk = std::max({pk, std::fabs(t.l[i]), std::fabs(t.r[i])}); finite &= std::isfinite(t.l[i]); }
            const float r = std::max(rms(seg(t.l, 0.0, 0.3)), rms(seg(t.l, 0.3, 1.2))); // percussivo ou ataque lento
            worstPeak = std::max(worstPeak, pk);
            quietest = std::min(quietest, r);
            if (r < 0.002f) { ok = false; bad += " nota" + std::to_string(note) + "/vel" + std::to_string(vel); }
        }
    expect(finite && ok, name, "toca em todo o teclado (C1..C8) e velocities 30/127, sem silencio nem NaN" + (bad.empty() ? "" : " -- silencio:" + bad));
    std::printf("      (pico maior %.2f, RMS menor %.4f)\n", worstPeak, quietest);
    auto big = play(p, chord({48, 52, 55, 59, 62, 65, 69, 72}, 0.0, 1.5), 2.0);
    float pk = 0; for (float v : big.l) pk = std::max(pk, std::fabs(v));
    expect(pk <= 1.0f && rms(big.l) > 0.01f, name, "acorde de 8 notas: soa e fica dentro do limite");
    // custo: 16 vozes, 1 s
    Synth s; s.init(48000.0, 64); s.part(0) = p; s.recompile(0);
    for (int n = 0; n < 16; ++n) s.noteOn(0, 40 + n * 2, 100);
    float L[256], R[256];
    auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < 48000 / 256; ++b) s.renderBlock(L, R, 256);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    expect(sec < 0.5, name, "16 vozes custam menos que meio nucleo");
    std::printf("      (16 vozes: %.0f%% de um nucleo, sem threads)\n", sec * 100);
}

int main(int argc, char** argv) {
    g_dir = argc > 1 ? argv[1] : "patches"; g_out = argc > 2 ? argv[2] : "output/patch_tests";
    fs::create_directories(g_out);
    std::map<std::string, PartParams> P;
    for (const auto& e : fs::directory_iterator(g_dir)) {
        PartParams pp;
        if (e.path().extension() == ".json" && loadPartFile(e.path().string(), pp)) P[pp.name] = pp;
    }
    std::printf("%zu patches carregados\n", P.size());
    auto has = [&](const char* n) { if (!P.count(n)) { std::printf("FALTA %s\n", n); ++g_fail; return false; } std::printf("\n== %s\n", n); return true; };

    if (has("Piano Eletrico FM")) {
        auto& p = P["Piano Eletrico FM"];
        auto t = play(p, chord({60}, 0.0, 2.0), 2.5); save("piano", t);
        float early = bright(seg(t.l, 0.005, 0.06), noteHz(60)), late = bright(seg(t.l, 1.0, 0.17), noteHz(60));
        expect(early > 3.0f * late, "", "o brilho do ataque (tine) some: harmonicos " + std::to_string(early).substr(0, 5) + " -> " + std::to_string(late).substr(0, 6));
        expect(std::fabs(semis(pitchHz(seg(t.l, 0.3, 0.1)), noteHz(60))) < 0.3f, "", "afinado (C4)");
        auto soft = play(p, chord({60}, 0.0, 1.0, 40), 1.0), hard = play(p, chord({60}, 0.0, 1.0, 127), 1.0);
        expect(rms(seg(hard.l, 0, 0.3)) > 1.5f * rms(seg(soft.l, 0, 0.3)) && bright(seg(hard.l, 0.005, 0.06), noteHz(60)) > 1.3f * bright(seg(soft.l, 0.005, 0.06), noteHz(60)),
               "", "tocar forte e mais alto e mais brilhante (velocity)");
        general("piano", p);
    }
    if (has("Baixo Filtrado")) {
        auto& p = P["Baixo Filtrado"];
        auto t = play(p, chord({36}, 0.0, 1.5), 2.0); save("baixo", t);
        float early = bright(seg(t.l, 0.005, 0.06), noteHz(36)), late = bright(seg(t.l, 0.8, 0.17), noteHz(36));
        expect(early > 3.0f * late, "", "o filtro fecha depois do ataque: harmonicos " + std::to_string(early).substr(0, 5) + " -> " + std::to_string(late).substr(0, 6));
        // legato: segunda nota com a primeira presa = glide (altura no meio do caminho logo depois)
        std::vector<Ev> leg = {{0.0, 36, 100, true}, {0.5, 43, 100, true}, {0.52, 36, 100, false}, {1.4, 43, 100, false}};
        auto g = play(p, leg, 1.6); save("baixo_glide", g);
        // tem sub-oitava (op2 razao 0.5): mede tudo pelo mesmo estimador e compara intervalos
        const float ref = zcHz(seg(g.l, 0.3, 0.1));
        float before = 0.0f, mid = semis(zcHz(seg(g.l, 0.515, 0.03)), ref), after = semis(zcHz(seg(g.l, 1.0, 0.1)), ref);
        expect(std::fabs(before) < 0.4f && std::fabs(after - 7) < 0.4f && mid > 0.5f && mid < 6.5f, "",
               "mono com glide: desliza de C2 a G2 (antes " + std::to_string(before).substr(0, 5) + ", meio " + std::to_string(mid).substr(0, 4) + ", depois " + std::to_string(after).substr(0, 5) + " semitons)");
        Synth s; s.init(48000.0, 8); s.part(0) = p; s.recompile(0); s.noteOn(0, 36, 100); s.noteOn(0, 40, 100);
        float L[64], R[64]; s.renderBlock(L, R, 64);
        expect(s.activeVoiceCount() == 1, "", "mono: duas teclas = uma voz");
        general("baixo", p);
    }
    if (has("Sino de Vidro")) {
        auto& p = P["Sino de Vidro"];
        auto t = play(p, chord({72}, 0.0, 3.0), 3.5); save("sino", t);
        const float f = noteHz(72);
        auto a = seg(t.l, 0.02, 0.17), b = seg(t.l, 2.0, 0.17);
        float inharm = bandEnergy(a, f * 2.70, f * 2.80), harm = bandEnergy(a, f * 1.95, f * 2.05);
        expect(inharm > 10 * harm, "", "inarmonico: tem o modo 2.76x, nao o harmonico 2x");
        float hiA = bandEnergy(a, f * 6, 20000) / bandEnergy(a, 20, 20000), hiB = bandEnergy(b, f * 6, 20000) / bandEnergy(b, 20, 20000);
        expect(hiA > 5 * hiB, "", "morph pro espectro escuro: parte aguda cai de " + std::to_string(hiA).substr(0, 5) + " pra " + std::to_string(hiB).substr(0, 7));
        expect(rms(seg(t.l, 2.0, 0.3)) > 0.003f, "", "ressoa por mais de 2 s");
        general("sino", p);
    }
    if (has("Pad Wavetable")) {
        auto& p = P["Pad Wavetable"];
        auto t = play(p, chord({48, 55, 60, 64}, 0.0, 5.5), 6.5); save("pad", t);
        expect(rms(seg(t.l, 0.0, 0.05)) < 0.5f * rms(seg(t.l, 1.0, 0.3)), "", "ataque lento (entra devagar)");
        auto solo = play(p, chord({48}, 0.0, 5.5), 6.0); // nota sozinha: o indice de harmonicos e relativo a uma fundamental
        std::vector<float> b; for (double s = 1.5; s < 5.5; s += 0.25) b.push_back(bright(seg(solo.l, s, 0.17), noteHz(48)));
        float bmin = *std::min_element(b.begin(), b.end()), bmax = *std::max_element(b.begin(), b.end());
        expect(bmax > 3.0f * bmin, "", "o timbre vai de brilhante a escuro com o LFO (harmonicos " + std::to_string(bmin).substr(0, 5) + " .. " + std::to_string(bmax).substr(0, 5) + ")");
        auto l = seg(t.l, 2, 0.5), r = seg(t.r, 2, 0.5);
        double lr = 0, ll = 0, rr = 0; for (size_t i = 0; i < l.size(); ++i) { lr += l[i] * r[i]; ll += l[i] * l[i]; rr += r[i] * r[i]; }
        const double corr = lr / std::sqrt(ll * rr);
        expect(corr < 0.95, "", "estereo largo (unison espalhado): correlacao L/R " + std::to_string(corr).substr(0, 4));
        general("pad", p);
    }
    if (has("Arpejo Pluck Comb")) {
        auto& p = P["Arpejo Pluck Comb"];
        auto t = play(p, chord({57, 60, 64}, 0.0, 3.0), 3.5); save("arpejo", t);
        auto on = onsets(t.l);
        std::printf("      (inicios: "); for (size_t i = 0; i < std::min<size_t>(on.size(), 8); ++i) std::printf("%.3f ", on[i]); std::printf("s)\n");
        bool grid = on.size() >= 16;
        for (size_t i = 0; grid && i + 1 < std::min<size_t>(on.size(), 12); ++i) { double d = on[i + 1] - on[i]; grid &= d > 0.09 && d < 0.17; }
        expect(grid, "", "um passo a cada ~1/16 (125 ms, com swing)");
        // alturas dos passos: sobe/desce em 2 oitavas sobre A3 C4 E4 (todas notas do acorde)
        int inChord = 0, steps = 0;
        for (size_t i = 0; i < std::min<size_t>(on.size(), 10); ++i) {
            float hz = pitchHz(seg(t.l, on[i] + 0.012, 0.06));
            float st = semis(hz, noteHz(57));
            int r = static_cast<int>(std::lround(st));
            int pc = ((r % 12) + 12) % 12;
            bool ok = std::fabs(st - r) < 0.35f && (pc == 0 || pc == 3 || pc == 7) && r >= 0 && r <= 19;
            inChord += ok; ++steps;
            std::printf("      passo %zu: %.1f Hz (%+.2f semitons de A3)\n", i, hz, st);
        }
        expect(inChord >= steps - 1, "", "cada passo e uma nota do acorde (afinada) -- comb com key track afinado");
        general("arpejo", p);
    }
    if (has("Algoritmos em Movimento")) {
        auto& p = P["Algoritmos em Movimento"];
        auto t = play(p, chord({45, 57}, 0.0, 4.5), 5.0); save("algoritmos", t);
        std::vector<float> c;
        for (double s = 0.3; s < 4.3; s += 0.0625) c.push_back(bright(seg(t.l, s, 0.06), noteHz(45)));
        float lo = *std::min_element(c.begin(), c.end()), hi = *std::max_element(c.begin(), c.end());
        // periodicidade: a cada 1/2 nota (1 s a 120 BPM) o morph volta
        double sameAt1s = 0, n = 0; for (size_t i = 0; i + 16 < c.size(); ++i) { sameAt1s += std::fabs(c[i] - c[i + 16]); ++n; }
        double diffAtHalf = 0; for (size_t i = 0; i + 8 < c.size(); ++i) diffAtHalf += std::fabs(c[i] - c[i + 8]);
        std::printf("      (harmonicos entre %.3f e %.3f; variacao media a 1 s %.3f, a 0.5 s %.3f)\n", lo, hi, sameAt1s / n, diffAtHalf / n);
        expect(hi > 2.0f * lo, "", "o timbre varre bastante com o morph de algoritmos");
        expect(sameAt1s < diffAtHalf, "", "o movimento se repete a cada 1/2 nota (envelope em loop sincronizado)");
        general("algoritmos", p);
    }
    if (has("Lead Dobrado")) {
        auto& p = P["Lead Dobrado"];
        auto t = play(p, chord({64}, 0.0, 2.0), 2.3); save("lead", t);
        float early = bright(seg(t.l, 0.005, 0.05), noteHz(64)), late = bright(seg(t.l, 1.0, 0.1), noteHz(64));
        expect(early > 1.5f * late, "", "a dobra abre no ataque e fecha (FX EG): harmonicos " + std::to_string(early).substr(0, 5) + " -> " + std::to_string(late).substr(0, 5));
        std::vector<float> pre, post;
        for (double s = 0.02; s < 0.11; s += 0.01) pre.push_back(semis(pitchNear(seg(t.l, s, 0.06), noteHz(64)), noteHz(64)));
        for (double s = 1.0; s < 1.6; s += 0.02) post.push_back(semis(pitchNear(seg(t.l, s, 0.06), noteHz(64)), noteHz(64)));
        auto spread = [](const std::vector<float>& v) { return *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end()); };
        expect(spread(pre) < 0.5f * spread(post) && spread(post) > 0.15f, "", "vibrato entra aos poucos (fade-in do atraso: variacao " + std::to_string(spread(pre)).substr(0, 4) +
               " -> " + std::to_string(spread(post)).substr(0, 4) + " semitons)");
        auto wheel = play(p, chord({64}, 0.0, 1.0), 1.0, 120.0f, [](Synth& s) { s.modWheel(0, 1.0f); });
        auto none = play(p, chord({64}, 0.0, 1.0), 1.0, 120.0f, [](Synth& s) { s.modWheel(0, 0.0f); });
        auto above4 = [](const std::vector<float>& x) { return bandEnergy(x, 4 * noteHz(64), 18000) / (bandEnergy(x, 20, 18000) + 1e-12f); };
        const float bw = above4(seg(wheel.l, 0.5, 0.17)), bn = above4(seg(none.l, 0.5, 0.17)); // dobrar mais leva o 3o harmonico pra cima
        expect(bw > 1.3f * bn, "", "mod wheel dobra mais (harmonicos " + std::to_string(bn).substr(0, 5) + " -> " + std::to_string(bw).substr(0, 5) + ")");
        general("lead", p);
    }
    if (has("Metais FM")) {
        auto& p = P["Metais FM"];
        auto t = play(p, chord({55, 60, 64}, 0.0, 1.5), 2.0); save("metais", t);
        auto one0 = play(p, chord({60}, 0.0, 1.0), 1.0);
        float c0 = bright(seg(one0.l, 0.005, 0.05), noteHz(60)), c1 = bright(seg(one0.l, 0.3, 0.1), noteHz(60));
        expect(c1 > 1.5f * c0, "", "o brilho cresce no ataque (harmonicos " + std::to_string(c0).substr(0, 5) + " -> " + std::to_string(c1).substr(0, 5) + ")");
        auto soft = play(p, chord({60}, 0.0, 1.0, 50), 1.0), hard = play(p, chord({60}, 0.0, 1.0, 127), 1.0);
        expect(bright(seg(hard.l, 0.4, 0.17), noteHz(60)) > 1.3f * bright(seg(soft.l, 0.4, 0.17), noteHz(60)), "", "velocity abre o brilho");
        auto one = play(p, chord({60}, 0.0, 1.0), 1.0);
        expect(std::fabs(semis(pitchHz(seg(one.l, 0.5, 0.1)), noteHz(60))) < 0.3f, "", "afinado depois do pitch EG do ataque");
        general("metais", p);
    }
    if (has("Coro de Vogais")) {
        auto& p = P["Coro de Vogais"];
        auto a = play(p, chord({50}, 0.0, 2.0), 2.2, 120.0f, [](Synth& s) { s.setMacro(0, 0, 0.0f); });
        auto o = play(p, chord({50}, 0.0, 2.0), 2.2, 120.0f, [](Synth& s) { s.setMacro(0, 0, 1.0f); });
        save("coro_a", a); save("coro_o", o);
        const float f = noteHz(50);
        // "a": energia em volta do 5o harmonico; "o": em volta do 3o
        float aRatio = bandEnergy(seg(a.l, 1, 0.17), f * 4.5, f * 5.5) / bandEnergy(seg(a.l, 1, 0.17), f * 2.5, f * 3.5);
        float oRatio = bandEnergy(seg(o.l, 1, 0.17), f * 4.5, f * 5.5) / bandEnergy(seg(o.l, 1, 0.17), f * 2.5, f * 3.5);
        expect(aRatio > 2.0f * oRatio, "", "Macro 1 \"Vogal\" troca o formante (h5/h3: " + std::to_string(aRatio).substr(0, 4) + " -> " + std::to_string(oRatio).substr(0, 4) + ")");
        auto chordT = play(p, chord({50, 57, 62}, 0.0, 4.0), 4.5); save("coro", chordT);
        general("coro", p);
    }
    if (has("Bleeps Sample-Hold")) {
        auto& p = P["Bleeps Sample-Hold"];
        auto t = play(p, chord({72}, 0.0, 3.0), 3.0); save("bleeps", t);
        int quantized = 0, total = 0, distinct = 0;
        float last = 99;
        for (int k = 2; k < 22; ++k) { // meio de cada 1/16 (125 ms)
            float st = semis(pitchHz(seg(t.l, k * 0.125 + 0.03, 0.06)), noteHz(72));
            ++total;
            quantized += std::fabs(st - std::round(st)) < 0.2f && std::fabs(st) <= 6.3f;
            distinct += std::fabs(st - last) > 0.5f;
            last = st;
        }
        expect(quantized >= total - 1 && distinct >= total / 2, "", "alturas aleatorias em semitons inteiros (+-6), trocando a cada 1/16 (" +
               std::to_string(quantized) + "/" + std::to_string(total) + " em semitom, " + std::to_string(distinct) + " trocas)");
        // portao: volume cai dentro de cada 1/16
        float startGate = rms(seg(t.l, 1.0 + 0.005, 0.02)), endGate = rms(seg(t.l, 1.0 + 0.095, 0.02));
        expect(startGate > 2 * endGate, "", "o envelope em loop recorta cada 1/16 (portao)");
        general("bleeps", p);
    }
    if (has("Orgao Drawbar")) {
        auto& p = P["Orgao Drawbar"];
        auto t = play(p, chord({48, 55, 60, 64, 67}, 0.0, 2.5), 3.0); save("orgao", t);
        std::vector<float> env;
        for (double s = 0.5; s < 2.4; s += 0.005) env.push_back(rms(seg(t.l, s, 0.01)));
        // frequencia do tremolo: pico do espectro da envoltoria entre 3 e 10 Hz
        float bestF = 0, bestM = 0;
        for (float f = 3; f <= 10; f += 0.05f) {
            double re = 0, im = 0; for (size_t i = 0; i < env.size(); ++i) { re += env[i] * std::cos(6.283 * f * i * 0.005); im += env[i] * std::sin(6.283 * f * i * 0.005); }
            float m = static_cast<float>(std::hypot(re, im)); if (m > bestM) { bestM = m; bestF = f; }
        }
        expect(std::fabs(bestF - 6.2f) < 0.4f, "", "tremolo a ~6.2 Hz (medido " + std::to_string(bestF).substr(0, 4) + " Hz)");
        auto l = seg(t.l, 1, 1), r = seg(t.r, 1, 1);
        float dl = rms(l), dr = rms(r);
        expect(std::fabs(dl - dr) / (dl + dr) < 0.2f, "", "pan do LFO global balanca em volta do centro");
        general("orgao", p);
    }
    if (has("Tambor de Membrana")) {
        auto& p = P["Tambor de Membrana"];
        auto t = play(p, chord({45}, 0.0, 0.3), 1.5); save("tambor", t);
        float p0 = pitchHz(seg(t.l, 0.005, 0.03)), p1 = pitchHz(seg(t.l, 0.25, 0.06));
        expect(p0 > p1 * 1.05f, "", "a afinacao cai no ataque (" + std::to_string(int(p0)) + " -> " + std::to_string(int(p1)) + " Hz)");
        expect(rms(seg(t.l, 1.2, 0.2)) < 0.1f * rms(seg(t.l, 0.02, 0.1)), "", "decai rapido (percussivo)");
        general("tambor", p);
    }
    std::printf("\n== %d ok, %d falharam\n", g_pass, g_fail);
    return g_fail;
}
