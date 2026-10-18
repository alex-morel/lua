MVerb -- reverb plate (topologia de Dattorro) de Martin Eastwood, 2010.
Origem: https://github.com/martineastwood/mverb (MVerb.h, commit do branch master
baixado em 2026-10-18). Licenca: GNU GPL v3 ou posterior (ver LICENSE).

Vendorizado SEM modificacoes. O dx7fm usa via src/dx7_globalfx.h (modo
"Plate" do bloco Reverb dos efeitos globais), que inclui <cstring>/<cmath>
antes deste header (ele usa memset/sin sem incluir nada) e silencia os
warnings dele.
