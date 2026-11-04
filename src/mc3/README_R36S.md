# Midnight Club: L.A. Remix — R36S / PortMaster Troubleshooting

Static recompilation of Midnight Club: L.A. Remix (PSP) via psprecomp.
Runs natively on R36S (RK3326 / Cortex-A53) via PortMaster — no emulator, no PS1 BIOS.

---

## Estrutura de arquivos esperada

```
/roms2/ports/
  Midnight Club - L.A. Remix.sh     ← wrapper de lançamento (raiz de ports/)
  midnight-club-lar/                ← subpasta do jogo
    port.json                       ← manifesto PortMaster versão 4
    mc3                             ← binário recompilado (39 MB)
    mc3.log                         ← log de execução (gerado automaticamente)
    mc3-debug.sh                    ← script de diagnóstico
    EBOOT.ELF                       ← executável descriptografado do PSP
    data/
      PSP_GAME/
        SYSDIR/
          EBOOT.ELF                 ← cópia dos dados do jogo
    cover.png
    screenshot.png
    gameinfo.xml
    README.md
```

---

## Problema: Jogo não aparece na lista do PortMaster

### Causa provável: `port.json` inválido ou formato errado

O PortMaster v4 espera um `port.json` com campos específicos:
- `version: 4`
- `name: "nome.zip"`
- `items: ["nome.sh", "subpasta"]`
- `attr.title`, `attr.genres`, etc.
- `files` mapeando os arquivos

**Solução:** Verifique se o `port.json` em `ports/midnight-club-lar/port.json` está no formato v4 completo. Compare com um porto que funciona (ex: `ports/celeste/port.json`).

**Não coloque o `.sh` dentro da subpasta.** Ele deve ficar na raiz de `ports/`:
```
/roms2/ports/Midnight Club - L.A. Remix.sh   ← CORRETO
/roms2/ports/midnight-club-lar/port.json     ← CORRETO
```

---

## Problema: Jogo abre e fecha imediatamente ("loading..." e nada)

### Causa 1: Biblioteca faltando no dArkOS

O dArkOS/dArkOSRE não inclui todas as bibliotecas que ports precisam. Isso é um problema conhecido na comunidade R36s.

**Diagnóstico:** Rode o script de diagnóstico:
```
/roms2/ports/midnight-club-lar/mc3-debug.sh
```

Ele vai reportar quais bibliotecas estão faltando (ex: `libSDL2.so`, `libEGL.so`, etc.).

**Solução:** Baixe o `dArkOSRE-Portmaster-Fix`:
- https://github.com/SjslTech/dArkOSRE-Portmaster-Fix/releases/tag/v1.0
- Ou peça no Discord do PortMaster (canal `#ports-help`).

### Causa 2: EBOOT.ELF ausente ou não descriptografado

O jogo precisa do `EBOOT.ELF` descriptografado em:
```
/roms2/ports/midnight-club-lar/data/PSP_GAME/SYSDIR/EBOOT.ELF
```

Se falta ou está criptografado, o jogo fecha imediatamente.

**Solução:** Verifique se o arquivo existe e tem cerca de 5 MB.

---

## Problema: Jogo inicia mas fica tela preta

### Causa: Framebuffer `/dev/fb0` não acessível

O mc3 desenha na memória do PSP e copia para `/dev/fb0` a cada frame. Se `/dev/fb0` não existir ou não tiver permissão, a tela fica preta.

**Diagnóstico:** 
```sh
ls -la /dev/fb0
```

**Solução:** Se `/dev/fb0` não existir, o driver de framebuffer não está carregado. Verifique o firmware do R36s.

---

## Problema: Jogo trava e não consegue sair

### Solução: SELECT + START

O jogo inclui um mecanismo de saída de emergência:
- Pressione **SELECT + START** simultaneamente para encerrar o jogo limpo.

### Saída automática

Se o GE (Graphics Engine) parar de enviar comandos por ~5 segundos (300 frames), o jogo encerra sozinho para não travar o console.

---

## Como obter logs para diagnóstico

### Via SSH no R36s:
```sh
ssh ark@<IP_DO_APARELHO>
cd /roms2/ports/midnight-club-lar
cat mc3.log
```

### Via PortMaster Discord

Se precisar de ajuda, cole o conteúdo de `mc3.log` no canal `#ports-help` do Discord do PortMaster:
https://discord.gg/portmaster

Inclua também:
- Versão do firmware (ArkOS, dArkOS, dArkOSRE, dArkOSen)
- Modelo do R36s (Genuíno, Plus, R36H, clone)
- Resultado do `mc3-debug.sh`

---

## Fontes e referências

- PortMaster: https://portmaster.games/
- PortMaster GitHub: https://github.com/PortsMaster/PortMaster-GUI
- PortMaster Discord: https://discord.gg/portmaster
- dArkOSRE Portmaster Fix: https://github.com/SjslTech/dArkOSRE-Portmaster-Fix
- R36S Community: https://r36s.org/
- Reddit R36S: https://www.reddit.com/r/R36S/

---

## Histórico de alterações

- v1.0: Versão inicial via psprecomp static recompilation
  - Software rendering via `/dev/fb0`
  - Saída automática se GE travar
  - Saída via SELECT + START
  - Script de diagnóstico integrado
