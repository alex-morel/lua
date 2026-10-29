#!/usr/bin/env python3
# [ADICIONADO em 2026-10-29] Gera as portas dos efeitos do Airwindows
# (github.com/airwindows/airwindows, Chris Johnson, licenca MIT) usadas no rack
# de efeitos: pra cada plugin, junta num header so
#   - o estado (membros privados do .h),
#   - a inicializacao (corpo do construtor do .cpp, sem a parte do VST),
#   - o processReplacing (float) do Proc.cpp, VERBATIM.
# A unica mudanca no codigo de audio e a semente do gerador do dither
# (rand() -> dx7fm::newRandomSeed(), que o golden fixa com fixRandomSeed()).
#
# Uso (fontes baixadas de plugins/LinuxVST/src/<Nome>/ numa pasta):
#   python3 tools/ports/airwindows_port.py PASTA_DAS_FONTES src/airwindows Drive Tube2 ...
import re
import sys
from pathlib import Path

HEADER = """#pragma once
// Porta do {name} do Airwindows (https://github.com/airwindows/airwindows,
// plugins/LinuxVST/src/{name}). Copyright (c) Chris Johnson / airwindows.
// Licenca MIT -- texto completo em THIRD_PARTY_NOTICES.md:
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software [...] THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
//   WARRANTY OF ANY KIND.
// GERADO por tools/ports/airwindows_port.py: estado, construtor e
// processReplacing originais, sem a camada VST. Unica mudanca: a semente do
// dither vem de dx7fm::newRandomSeed() em vez de rand().
#include "../dx7_dsp.h"
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <math.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

namespace dx7fm::airwindows {{

class {name} {{
public:
    using VstInt32 = int32_t;
    // parametros 0..1 (nomes no plugin: {pnames})
{params}
    {name}() {{ reset(); }}
    void setSampleRate(double sr) {{ sampleRate_ = sr; }}
    double getSampleRate() const {{ return sampleRate_; }}
    // estado inicial (construtor original, sem os parametros)
    void reset() {{
{init}
    }}

    void processReplacing(float** inputs, float** outputs, VstInt32 sampleFrames)
{body}

private:
    double sampleRate_ = 44100.0;
{members}
}};

}} // namespace dx7fm::airwindows

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
"""


def matching_brace(text, start):
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("chave sem par")


def port(src: Path, name: str) -> str:
    h = (src / f"{name}.h").read_text(errors="replace")
    cpp = (src / f"{name}.cpp").read_text(errors="replace")
    proc = (src / f"{name}Proc.cpp").read_text(errors="replace")

    # membros: depois de "_canDo;" ate o fim da classe; parametros (float A..Z) a parte
    priv = h[h.index("_canDo;") + len("_canDo;"):]
    priv = priv[:priv.index("};")]
    members, param_names = [], []
    for line in priv.splitlines():
        m = re.match(r"\s*float\s+([A-Z]);", line)
        if m:
            param_names.append(m.group(1))
            continue
        if line.strip():
            members.append("    " + line.strip())

    # construtor: corpo ate o primeiro _canDo.insert
    sig = cpp.index(f"{name}::{name}(")
    body_start = cpp.index("{", sig)
    ctor = cpp[body_start + 1:matching_brace(cpp, body_start)]
    ctor = ctor[:ctor.index("_canDo.insert")] if "_canDo.insert" in ctor else ctor
    defaults, init = {}, []
    for line in ctor.splitlines():
        s = line.strip()
        m = re.match(r"([A-Z])\s*=\s*([-0-9.]+)\s*;", s)
        if m and m.group(1) in param_names:
            defaults[m.group(1)] = m.group(2)
            continue
        if not s:
            continue
        s = re.sub(r"(fpd[LR]?)\s*=\s*1\.0\s*;\s*while\s*\(\s*\1\s*<\s*16386\s*\)\s*\1\s*=\s*rand\(\)\s*\*\s*UINT32_MAX\s*;",
                   r"\1 = dx7fm::newRandomSeed() | 16386u;", s)
        init.append("        " + s)

    # getParameterName: nomes pra documentacao
    pn = {}
    gpn = cpp.index("::getParameterName")
    gpn_body = cpp[cpp.index("{", gpn):matching_brace(cpp, cpp.index("{", gpn)) + 1]
    for m in re.finditer(r"case kParam([A-Z]):\s*vst_strncpy\s*\(\s*text,\s*\"([^\"]*)\"", gpn_body):
        pn[m.group(1)] = m.group(2)

    # processReplacing verbatim
    pr = proc.index(f"void {name}::processReplacing")
    pb = proc.index("{", pr)
    body = proc[pb:matching_brace(proc, pb) + 1]
    body = "\n".join("    " + l if l.strip() else "" for l in body.splitlines())

    params = "\n".join(f"    float {p} = {defaults.get(p, '0.0')}f;" for p in param_names)
    pnames = ", ".join(f"{p}={pn.get(p, '?')}" for p in param_names)
    return HEADER.format(name=name, params=params, pnames=pnames, init="\n".join(init), body=body, members="\n".join(members))


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for name in sys.argv[3:]:
        (out / f"{name}.h").write_text(port(src, name))
        print("gerado", out / f"{name}.h")


if __name__ == "__main__":
    main()
