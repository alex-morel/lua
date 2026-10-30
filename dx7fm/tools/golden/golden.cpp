// [ADICIONADO em 2026-10-28] Golden: renderiza cenarios deterministicos do
// motor e compara a saida amostra a amostra entre duas versoes do codigo.
// Serve pra provar que uma otimizacao/refatoracao NAO muda o som (ou mostrar
// exatamente quais cenarios mudaram, quando a mudanca e de proposito).
//
//   ./build/golden render PASTA     grava PASTA/<cenario>.f32 (L,R intercalados)
//   ./build/golden diff A B         compara as duas pastas; sai 1 se algo mudou
//
// tools/golden/compare.sh REF faz o ciclo todo contra um commit (REF precisa
// ja ter este arquivo). Os cenarios "fabrica_*" vem de app/factory_bank.h:
// mudar um patch de fabrica muda o golden dele, e isso e esperado.
#include "dx7_alloc.h"
#include "fx_factory.h"
#include "factory_bank.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
using namespace dx7fm;
namespace fs = std::filesystem;

namespace {

struct Scenario {
    std::string name;
    std::function<void(std::vector<float>&)> render; // saida L,R intercalada
};

// toca com eventos a cada bloco de 64 amostras: f(synth, bloco) antes de cada bloco
void run(std::vector<float>& out, Synth& s, int blocks, const std::function<void(Synth&, int)>& f) {
    float L[64], R[64];
    for (int b = 0; b < blocks; ++b) {
        if (f) f(s, b);
        s.renderBlock(L, R, 64);
        for (int i = 0; i < 64; ++i) { out.push_back(L[i]); out.push_back(R[i]); }
    }
}

std::vector<Scenario> scenarios() {
    std::vector<Scenario> v;
    // motor puro, patch padrao
    v.push_back({"padrao_acorde", [](std::vector<float>& o) {
        Synth s; s.init(48000.0, 16); s.recompile(0);
        run(o, s, 1500, [](Synth& s, int b) {
            if (b == 0) for (int n : {48, 55, 60, 64}) s.noteOn(0, n, 100);
            if (b == 300) s.noteOn(0, 67, 60);
            if (b == 900) for (int n : {48, 55, 60, 64, 67}) s.noteOff(0, n);
        });
    }});
    v.push_back({"padrao_controles", [](std::vector<float>& o) { // bend, mod wheel, aftertouch, sustain
        Synth s; s.init(48000.0, 16); s.recompile(0);
        run(o, s, 1500, [](Synth& s, int b) {
            if (b == 0) s.noteOn(0, 57, 110);
            if (b >= 100 && b < 400) s.pitchBend(0, 2.0f * (b - 100) / 300.0f); // semitons
            if (b == 400) s.modWheel(0, 0.8f);
            if (b == 500) s.channelAftertouch(0, 0.6f);
            if (b == 600) { s.sustainPedal(0, true); s.noteOff(0, 57); }
            if (b == 1000) s.sustainPedal(0, false);
        });
    }});
    v.push_back({"padrao_multitimbral", [](std::vector<float>& o) {
        Synth s; s.init(48000.0, 32);
        auto bank = dx7fm_app::factory::factoryBank();
        for (int p = 0; p < kNumParts; ++p) {
            s.part(p) = bank[static_cast<size_t>(p * 3) % bank.size()].part;
            s.part(p).enabled = true;
            s.recompile(p);
        }
        run(o, s, 1500, [](Synth& s, int b) {
            if (b % 200 == 0) for (int p = 0; p < kNumParts; ++p) s.noteOn(p, 48 + p * 5 + (b / 200) % 3, 90);
            if (b % 200 == 150) for (int p = 0; p < kNumParts; ++p) s.noteOff(p, 48 + p * 5 + (b / 200) % 3);
        });
    }});
    // [2026-10-29] cada efeito do rack num slot da cadeia 1, sobre o patch padrao
    for (int k = 1; k < static_cast<int>(FxKind::Count); ++k) {
        const FxKind kind = static_cast<FxKind>(k);
        v.push_back({std::string("rack_") + fxInfo(kind).id, [kind](std::vector<float>& o) {
            Synth s; s.init(48000.0, 8); s.recompile(0);
            FxRackParams r;
            r.chain[0].slot[0].setKind(kind);
            s.setFxRack(r);
            run(o, s, 375, [](Synth& s, int b) {
                if (b == 0) s.noteOn(0, 57, 110);
                if (b == 60) s.noteOff(0, 57);
            });
        }});
    }
    // [2026-10-30] presets de efeitos de fabrica (cobrem os algoritmos de roteamento)
    for (const auto& f : dx7fm_app::fxfactory::fxFactoryBank()) {
        std::string n = "rackpreset_";
        for (char c : f.name) n += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
        v.push_back({n, [f](std::vector<float>& o) {
            Synth s; s.init(48000.0, 8);
            for (int p = 0; p < 2; ++p) { s.part(p).enabled = true; s.recompile(p); }
            s.setFxRack(f.rack);
            run(o, s, 375, [](Synth& s, int b) {
                if (b == 0) { s.noteOn(0, 57, 110); s.noteOn(1, 64, 100); }
                if (b == 60) { s.noteOff(0, 57); s.noteOff(1, 64); }
            });
        }});
    }
    // banco de fabrica: a previa de cada preset
    for (const auto& f : dx7fm_app::factory::factoryBank()) {
        std::string n = "fabrica_";
        for (char c : f.part.name) n += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
        v.push_back({n, [f](std::vector<float>& o) {
            std::vector<float> L, R;
            dx7fm_app::factory::renderPreview(f, f.part, L, R);
            for (size_t i = 0; i < L.size(); ++i) { o.push_back(L[i]); o.push_back(R[i]); }
        }});
    }
    return v;
}

int render(const fs::path& dir) {
    fs::create_directories(dir);
    for (const auto& sc : scenarios()) {
        std::vector<float> out;
        fixRandomSeed(12345); // cada cenario comeca da mesma semente
        sc.render(out);
        std::ofstream f(dir / (sc.name + ".f32"), std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size() * sizeof(float)));
        std::printf("%-40s %zu amostras\n", sc.name.c_str(), out.size() / 2);
    }
    return 0;
}

std::vector<float> load(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<float> v(b.size() / sizeof(float));
    std::memcpy(v.data(), b.data(), v.size() * sizeof(float));
    return v;
}

int diff(const fs::path& a, const fs::path& b) {
    int changed = 0, same = 0;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(a)) if (e.path().extension() == ".f32") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& fa : files) {
        const fs::path fb = b / fa.filename();
        const std::string name = fa.stem().string();
        if (!fs::exists(fb)) { std::printf("  [so em A]  %s\n", name.c_str()); continue; }
        auto x = load(fa), y = load(fb);
        if (x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0) { ++same; continue; }
        ++changed;
        double maxd = 0, first = -1;
        for (size_t i = 0; i < std::min(x.size(), y.size()); ++i) {
            const double d = std::fabs(static_cast<double>(x[i]) - y[i]);
            if (d > 0 && first < 0) first = static_cast<double>(i / 2) / 48000.0;
            maxd = std::max(maxd, d);
        }
        std::printf("  [MUDOU]    %-38s dif. max %.2e, desde %.4f s%s\n", name.c_str(), maxd, first,
                    x.size() != y.size() ? " (tamanho diferente)" : "");
    }
    for (const auto& e : fs::directory_iterator(b))
        if (e.path().extension() == ".f32" && !fs::exists(a / e.path().filename())) std::printf("  [so em B]  %s\n", e.path().stem().c_str());
    std::printf("%d identicos, %d mudaram\n", same, changed);
    return changed ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "render") == 0) return render(argv[2]);
    if (argc == 4 && std::strcmp(argv[1], "diff") == 0) return diff(argv[2], argv[3]);
    std::fprintf(stderr, "uso: golden render PASTA | golden diff PASTA_A PASTA_B\n");
    return 2;
}
