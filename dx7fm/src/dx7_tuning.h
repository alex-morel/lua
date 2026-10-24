#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

// [ADICIONADO em 2026-10-24] Microafinacao no formato Scala
// (https://www.huygens-fokker.org/scala/scl_format.html):
//  .scl -- descricao, numero de graus N e N alturas (cents se tem ".",
//          senao razao "a/b" ou inteiro); o ultimo grau e o periodo (a oitava).
//  .kbm -- mapeamento teclado -> graus: tamanho do mapa, primeira/ultima
//          nota, nota do meio (grau 0), nota e frequencia de referencia, grau
//          da "oitava formal" e o mapa (indice -> grau, "x" = tecla sem som).
// Sem .kbm: mapa linear com a nota 60 no grau 0 e a nota 69 em `a4Hz`.
// TuningTable e montada na thread da UI (Synth::recompile) e lida pelo audio.
namespace dx7fm {

struct TuningTable {
    float hz[128];
    bool mapped[128];
};

struct Scale {
    std::string description;
    std::vector<double> cents; // graus 1..N (o ultimo = periodo)
};

struct KeyMap {
    int size = 0, first = 0, last = 127, middle = 60, refNote = 69;
    double refHz = 440.0;
    int octaveDegree = 0; // 0 = o periodo da escala
    std::vector<int> map; // grau por posicao; -1 = sem som
};

namespace tuning_detail {
// linhas uteis (sem comentarios "!"); trim nas pontas
inline std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty() && l[0] == '!') continue;
        out.push_back(l);
    }
    return out;
}
inline std::string firstToken(const std::string& l) {
    std::istringstream in(l);
    std::string t;
    in >> t;
    return t;
}
} // namespace tuning_detail

// false + err se o arquivo e invalido
inline bool parseScl(const std::string& text, Scale& out, std::string& err) {
    auto ls = tuning_detail::lines(text);
    if (ls.size() < 2) { err = "arquivo .scl incompleto"; return false; }
    out.description = ls[0];
    const std::string countTok = tuning_detail::firstToken(ls[1]);
    char* end = nullptr;
    long n = std::strtol(countTok.c_str(), &end, 10);
    if (countTok.empty() || *end || n < 1 || n > 1024) { err = "numero de notas invalido: \"" + countTok + "\""; return false; }
    if (static_cast<long>(ls.size()) < 2 + n) { err = "menos alturas que o numero de notas"; return false; }
    out.cents.clear();
    for (long i = 0; i < n; ++i) {
        const std::string t = tuning_detail::firstToken(ls[static_cast<size_t>(2 + i)]);
        double c;
        if (t.find('.') != std::string::npos) {
            c = std::strtod(t.c_str(), &end);
            if (*end) { err = "altura invalida: \"" + t + "\""; return false; }
        } else {
            const size_t slash = t.find('/');
            double num = std::strtod(t.substr(0, slash).c_str(), &end);
            double den = slash == std::string::npos ? 1.0 : std::strtod(t.substr(slash + 1).c_str(), nullptr);
            if (t.empty() || num <= 0.0 || den <= 0.0) { err = "razao invalida: \"" + t + "\""; return false; }
            c = 1200.0 * std::log2(num / den);
        }
        out.cents.push_back(c);
    }
    if (out.cents.back() <= 0.0) { err = "o ultimo grau (periodo) precisa ser maior que 0"; return false; }
    return true;
}

inline bool parseKbm(const std::string& text, KeyMap& out, std::string& err) {
    std::vector<std::string> toks;
    for (const auto& l : tuning_detail::lines(text)) {
        std::string t = tuning_detail::firstToken(l);
        if (!t.empty()) toks.push_back(t);
    }
    if (toks.size() < 7) { err = "arquivo .kbm incompleto"; return false; }
    out.size = std::atoi(toks[0].c_str());
    out.first = std::clamp(std::atoi(toks[1].c_str()), 0, 127);
    out.last = std::clamp(std::atoi(toks[2].c_str()), 0, 127);
    out.middle = std::atoi(toks[3].c_str());
    out.refNote = std::clamp(std::atoi(toks[4].c_str()), 0, 127);
    out.refHz = std::strtod(toks[5].c_str(), nullptr);
    out.octaveDegree = std::atoi(toks[6].c_str());
    if (out.size < 0 || out.size > 1024 || out.refHz <= 0.0) { err = "cabecalho do .kbm invalido"; return false; }
    out.map.clear();
    for (int i = 0; i < out.size; ++i) {
        if (7 + static_cast<size_t>(i) >= toks.size()) { out.map.push_back(-1); continue; } // mapa curto: o resto sem som
        const std::string& t = toks[7 + static_cast<size_t>(i)];
        out.map.push_back(t == "x" || t == "X" ? -1 : std::atoi(t.c_str()));
    }
    return true;
}

// 12-TET com A4 = a4Hz: MESMA conta de Voice::noteToHz (com a4Hz = 440 a
// tabela e bit a bit igual ao motor sem afinacao)
inline TuningTable equalTemperament(float a4Hz = 440.0f) {
    TuningTable t;
    for (int n = 0; n < 128; ++n) {
        t.hz[n] = a4Hz * std::pow(2.0f, (n - 69) / 12.0f);
        t.mapped[n] = true;
    }
    return t;
}

// escala + mapa (kbm nulo = linear, nota 60 = grau 0, nota 69 = a4Hz)
inline TuningTable buildTuning(const Scale& scale, const KeyMap* kbm, float a4Hz) {
    const int N = static_cast<int>(scale.cents.size());
    const double period = scale.cents.back();
    auto degreeCents = [&](long deg) { // grau qualquer (inclusive negativo ou >= N)
        long oct = deg >= 0 ? deg / N : -((-deg + N - 1) / N);
        long idx = deg - oct * N;
        return oct * period + (idx == 0 ? 0.0 : scale.cents[static_cast<size_t>(idx - 1)]);
    };
    KeyMap linear;
    linear.refHz = a4Hz;
    const KeyMap& km = kbm ? *kbm : linear;
    const double formalOctave = km.octaveDegree > 0 ? degreeCents(km.octaveDegree) : period;
    // cents (relativo a nota do meio) de cada nota; NaN = sem som
    auto noteCents = [&](int n, bool& ok) {
        ok = n >= km.first && n <= km.last;
        const long d = n - km.middle;
        if (km.size == 0) return degreeCents(d); // mapa linear: cada tecla = um grau
        const long rep = d >= 0 ? d / km.size : -((-d + km.size - 1) / km.size);
        const long pos = d - rep * km.size;
        const int deg = km.map[static_cast<size_t>(pos)];
        if (deg < 0) { ok = false; return 0.0; }
        return rep * formalOctave + degreeCents(deg);
    };
    bool refOk;
    const double refCents = noteCents(km.refNote, refOk);
    TuningTable t;
    for (int n = 0; n < 128; ++n) {
        bool ok;
        const double c = noteCents(n, ok);
        t.mapped[n] = ok;
        t.hz[n] = ok ? static_cast<float>(km.refHz * std::pow(2.0, (c - refCents) / 1200.0)) : 0.0f;
    }
    return t;
}

} // namespace dx7fm
