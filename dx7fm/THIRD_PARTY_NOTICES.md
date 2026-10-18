# Componentes de terceiros e licenças

O dx7fm inclui código e algoritmos portados de projetos open source. Como dois
deles são GPL (MVerb, GPLv3; ZamComp, GPLv2 ou posterior), **o dx7fm como um
todo é distribuído sob a GNU GPL versão 3** (texto completo em `LICENSE`).
Quem distribuir o programa (binário ou fonte) precisa oferecer o código-fonte
completo sob a GPLv3.

| Componente | Onde é usado | Autor | Licença | Forma |
|---|---|---|---|---|
| Dear ImGui | interface do app (`third_party/imgui`) | Omar Cornut | MIT | vendorizado sem mudanças |
| MVerb | reverb "Plate" (`third_party/mverb/MVerb.h`) | Martin Eastwood | GPLv3+ | vendorizado sem mudanças |
| ZamComp | compressor (`src/dx7_globalfx.h`, `ZamCompressor`) | Damien Zammit | GPLv2+ | porta (C++), com mudanças |
| Rings — Ensemble | chorus (`EnsembleChorus`) | Emilie Gillet (Mutable Instruments) | MIT | porta, com mudanças |
| Clouds — Reverb/FxEngine | reverb "Hall" (`CloudsHall`) | Emilie Gillet (Mutable Instruments) | MIT | porta, com mudanças |
| Freeverb | reverb "Room" (`FreeverbRoom`) | Jezar at Dreampoint | domínio público | porta fiel |
| Faust phaflangers.lib | phaser e flanger (`Phaser`, `Flanger`) | Julius O. Smith III | STK-4.3 (estilo MIT) | porta, com mudanças |
| Audio EQ Cookbook | EQ de 3 bandas (`Eq3`) | Robert Bristow-Johnson | fórmulas publicadas | implementação própria |
| Roboto Medium | fonte da interface (`third_party/imgui/misc/fonts/Roboto-Medium.ttf`) | Google (Christian Robertson) | Apache 2.0 | arquivo distribuído com o Dear ImGui, carregado em tempo de execução |

As mudanças feitas em cada porta estão descritas no comentário de cabeçalho
do bloco correspondente em `src/dx7_globalfx.h`.

## Origem dos arquivos (baixados em 2026-10-18)

- MVerb: <https://github.com/martineastwood/mverb> (`MVerb.h`, `LICENSE`)
- ZamComp: <https://github.com/zamaudio/zam-plugins> (`plugins/ZamComp/ZamCompPlugin.cpp`)
- Mutable Instruments: <https://github.com/pichenettes/eurorack> (`rings/dsp/fx/ensemble.h`,
  `clouds/dsp/fx/reverb.h`, `clouds/dsp/fx/fx_engine.h`; parâmetros do reverb conforme
  `clouds/dsp/granular_processor.cc`) e <https://github.com/pichenettes/stmlib>
  (`dsp/cosine_oscillator.h`, só como referência do LFO)
- Freeverb (distribuição original de Jezar): <https://github.com/chiptunes-swift/freeverb>
  (`freeverb/Components/*`)
- Faust: <https://github.com/grame-cncm/faustlibraries> (`phaflangers.lib`, `licenses/stk-4.3.0.md`)

---

## Textos de licença e avisos exigidos

### Mutable Instruments (Rings, Clouds) — MIT

```
Copyright 2014 Emilie Gillet.

Author: Emilie Gillet (emilie.o.gillet@gmail.com)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### Faust phaflangers.lib (funções de Julius O. Smith III) — STK-4.3

Copyright (C) 2003-2017 Julius O. Smith III, liberado sob a licença STK-4.3:

```
The Synthesis ToolKit in C++ (STK)
Copyright (c) 1995--2017 Perry R. Cook and Gary P. Scavone

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

Any person wishing to distribute modifications to the Software is
asked to send the modifications to the original developer so that they
can be incorporated into the canonical version.  This is, however, not
a binding provision of this license.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### Freeverb — domínio público

```
Written by Jezar at Dreampoint, June 2000
http://www.dreampoint.co.uk
This code is public domain
```

### ZamComp — GNU GPL v2 ou posterior

```
ZamComp mono compressor
Copyright (C) 2014  Damien Zammit <damien@zamaudio.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of
the License, or (at your option) any later version.
```

Usado aqui sob a opção "versão posterior", ou seja, GPLv3 (ver `LICENSE`).

### MVerb — GNU GPL v3 ou posterior

```
Copyright (c) 2010 Martin Eastwood
This code is distributed under the terms of the GNU General Public License
```

Texto completo em `third_party/mverb/LICENSE` (idêntico a `LICENSE`).

### Dear ImGui — MIT

Ver `third_party/imgui/LICENSE.txt`.

### Roboto — Apache License 2.0

A fonte Roboto (Copyright Google, desenhada por Christian Robertson) é
licenciada sob a Apache License, Version 2.0
(<https://www.apache.org/licenses/LICENSE-2.0>), compatível com a GPLv3. O app
carrega o arquivo `Roboto-Medium.ttf` que vem com o Dear ImGui; sem ele, usa a
fonte embutida do ImGui.
