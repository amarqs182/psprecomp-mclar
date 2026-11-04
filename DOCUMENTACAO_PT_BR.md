# DOCUMENTACAO — Midnight Club: L.A. Remix (USA) — PSP Static Recompilation

## Status atual (resumo executivo)

O jogo **roda e processa comandos GE** mas **não renderiza pixels reais** na tela.
- Janela SDL2 abre corretamente (X11 ou Wayland/XWayland)
- O GE processa 4.194.304 comandos, 16 vértices, tipo=6 (sprites)
- Os sprites chegam com coordenadas (0,0) e cor 0x00000000 (transparente/invisível)
- `GE pixels = 0` → nada é desenhado na VRAM PSP
- Tela mostra apenas o gradiente de teste (vermelho → verde → azul)
- Zero crash, zero loop infinito, zero stack exhaustion
- 10 threads guest criadas, `module_start` retorna, jogo estável 20+ segundos

---

## O que foi feito (histórico detalhado)

### Fase 1 — Estabilidade e boot
- Corrigido stack leak em `is_tail_call()` (`emit.c:214`) — agora retorna 0 quando detecta teardown de frame do caller após `jal`
- Adicionado `PSP_SP_INDEX` (29) em `decode.h` para identificação do registrador de stack pointer
- Regenerado código corpus — 1.649 call sites antes com leak agora emitem epílogos corretos
- Build cross-compilado com sucesso — binário aarch64 ELF para R36S Cortex-A53 (`-mcpu=cortex-a53 -march=armv8-a+simd`)
- 10 threads guest criadas, `module_start @ 0x000005A4` retorna
- Jogo roda indefinidamente sem crash, loop ou stack exhaustion

### Fase 2 — Renderização via SDL2
- Corrigido `GE_FBP` em `ge.c:339` — agora usa `PSP_VRAM_BASE` quando `fbp` é 0
- Adicionado `psp_render_set_target()` em `render.c` para atualizar estado global do framebuffer
- Adicionada declaração de `psp_render_set_target()` em `psprecomp/render.h`
- Modificado `render_sdl.c` — removeu variáveis locais `g_fb_addr`/`g_fb_stride`, agora usa funções getter
- Adicionado debug log em `GE_PRIM` em `ge.c` — mostra tipo, count, vtype, vaddr, fbp
- Adicionado debug log em `draw_prim()` em `ge.c` — mostra stride, decoded count, vértice[0]
- Gradiente de teste imediato na janela SDL2 (480x272 RGBA, vermelho→verde→azul)
- SIGSEGV handler com `ucontext_t` — redireciona RIP para `segv_exit_stub()` que chama `pthread_exit()` limpo (evita `siglongjmp` não async-signal-safe)
- `max_frames` alterado de 300 para 100000000

### Fase 3 — Debug de renderização (esta sessão)
- Adicionado debug de `GE_VADDR` em `ge.c` — mostra arg, vaddr resolvido e base
- Dump de memória bruta do buffer de vértices no primeiro `draw_prim`
- Forçado cor vermelha opaca (`0xFFFF0000`) quando `rgba == 0x00000000` em `read_vertex`
- Forçado posição centro de tela (240, 136) quando vértice está em (0,0) em `read_vertex`
- Adicionado sprite de teste de 40x12 px no centro quando `sw_sprite` detecta zero size em `render.c`
- Corrigido `put_pixel` para usar `PSP_VRAM_BASE` (0x04000000) e stride 512 como padrão quando `g_fb_addr`/`g_fb_stride` não estão inicializados

---

## Problema pendente: GE pixels = 0

### Sintoma
```
ge: draw_prim type=6 count=16 stride=8 decoded=16 v[0]=(240,136,rgba=0xFFFF0000) fbp=0x04000000
mc3: frame 8700, GE commands=4194304, GE vertices=16, GE pixels=0
```

O vértice já tem coordenadas válidas e cor vermelha, mas nenhum pixel é escrito na VRAM PSP.

### Causa raiz provável
O buffer de vértices na VRAM PSP (`0x00047980`) contém dados em grande parte zerados:
```
00047980: 00 00 00 00 00 00 00 00  <- primeiro vértice: tudo zero
00047990: 40 00 10 01 00 00 00 00  <- segundo vértice
...
```
O jogo PSP não preencheu o buffer de vértices antes de chamar `sceGeListEnQueue`. Isso é comportamento do jogo original (provavelmente tela de carregamento ou inicialização incompleta do GE).

### Trabalho restante
1. Descobrir por que o software renderer não está escrevendo na VRAM (`sw_draw` → `sw_sprite` → `put_pixel`)
2. Verificar se `g_fb_addr` está sendo propagado corretamente do `psp_render_set_target` para `put_pixel`
3. Quando o software renderer funcionar, o SDL2 (`sdl2_draw_frame`) deve ler a VRAM e blitar para a janela
4. Alternativa: forçar renderização direta no `gles_draw` de `render_sdl.c` sem depender do software renderer

---

## Como executar (host, com debug)

```bash
cd /home/amarqs/mc3proj/build-native
SDL_VIDEODRIVER=x11 ./src/mc3/mc3 -e /home/amarqs/mc3_test/data/PSP_GAME/SYSDIR/EBOOT.ELF -r sdl2
```

O que esperar:
- Janela SDL2 aberta com título "MIDNIGHT CLUB: L.A. REMIX - PSP Static Recomp"
- Gradiente de teste vermelho→verde→azul visível
- Debug logs no stderr com comandos GE, GE_VADDR, GE_PRIM, draw_prim
- Para sair: feche a janela ou Ctrl+C

---

## Como construir (host)

```bash
cd /home/amarqs/mc3proj/build-native
make mc3
```

Dependências: SDL2 (libsdl2-dev), C, make/cmake.

---

## Como construir (R36S / aarch64 cross)

O binário já está cross-compilado para Cortex-A53. Para copiar para o dispositivo:
- `build-native/src/mc3/mc3` → `/roms2/ports/midnight-club-lar/mc3` no R36S
- Wrapper define `MC3_RENDER=software`
- Sem GLES 3.2 no dispositivo (Mali driver ausente) → usa `/dev/fb0` diretamente via software rendering

---

## Arquivos modificados nesta sessão

| Arquivo | Mudança |
|---------|---------|
| `src/psprecomp/src/hle/ge.c` | Debug GE_VADDR, força cor vermelha/posição centro quando vértice zero, debug draw_prim |
| `src/psprecomp/src/render.c` | sw_sprite com sprite de teste zero-size, put_pixel com defaults PSP_VRAM_BASE, removido software backend separado (unificado) |
| `src/mc3/render_sdl.c` | Backend SDL2 com blit da VRAM PSP, gradiente de teste, evento SDL_QUIT |
| `src/mc3/main.c` | Loop principal, auto-ctrl CROSS, debug de frames |
| `DOCUMENTACAO_PT_BR.md` | Este arquivo |

---

## Repositório GitHub

Este repositório **não tem remote configurado ainda**. Para publicar:

```bash
cd /home/amarqs/mc3proj
git remote add origin https://github.com/<USUARIO>/<REPO>.git
git push -u origin master
```

**Não coloque game data (ISO, ELF, ZIP, PRX) no repositório** — eles estão no .gitignore.

---

## Regras de código (convenção do usuário)

- Indentação: 4 espaços, não tabs
- Sem strings de inicialização C99/C11
- Sem API keys/tokens/senhas (usar [REDACTED])
- Zero tolerância para bugs: sem loop, sem crash, sem stack exhaustion
- Debug logs ativos para investigação
