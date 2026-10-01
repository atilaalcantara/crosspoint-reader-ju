# Desenvolvimento e simulador X4 Pro

## Pré-requisitos

- macOS com SDL2 e `sdl2-config` no `PATH` (`brew install sdl2`).
- PlatformIO CLI (`pio`) e Python disponíveis no `PATH`.
- Dependências PlatformIO baixadas durante o primeiro build.

## Build e execução

Na raiz de `crosspoint-reader-ju/`:

```bash
bash run-simulator.sh
```

O script cria `simulator-sd/` na raiz do projeto na primeira execução, compila
se necessário e abre o simulador X4 Pro. Essa pasta é um atalho para o mesmo
cartão persistente dos testes (`build/simulator-test-sd/`). O conteúdo **não é
apagado** ao fechar ou reiniciar. Copie os arquivos para:

```text
simulator-sd/books/MeuLivro.epub
simulator-sd/study/MeuBaralho.cps
simulator-sd/study/MeuBaralho.media/   (quando o deck tiver imagens)
```

Para ver arquivos novos em Study ou na biblioteca, feche e rode o mesmo script
novamente; o progresso, destaques e notas continuarão no mesmo cartão. O
comando `python3 tools/simulator-test/prepare_sd.py --reset` limpa o cartão de
testes e **não deve ser usado** para uma reinicialização normal.

Para apenas compilar: `pio run -c platformio.sim.ini -e simulator_x4`.

## Modo restrito (aproximação do hardware)

`bash run-simulator.sh --strict-hardware` limita o heap reportado ao firmware
(200 KB livres, 64 KB por alocação — ordem de grandeza do X4 Pro) e o
simulador aborta `close()` sem `open()`, como o assert do hardware. Isso pega
bugs de ciclo de vida de arquivos e estouros de buffers que o modo normal
perdoa. Não é prova de hardware: CPU, SDMMC, temporização e refresh e-ink
continuam diferentes do aparelho.

## Perfil, tela e input

O ambiente `simulator_x4` define `SIMULATOR_DEVICE_X4_PRO`: usa o perfil X4 Pro
do simulador, framebuffer de 800×480, touch, tecla capacitiva Home, frontlight,
inversão de display e RTC simulados. Mouse representa toque: clique/tap e arraste
representam toque e swipe. Teclas: ↑/↓ navegam pelos botões laterais, ←/→ pelos
botões frontais, Return confirma, Escape volta, P liga/desliga, S simula sleep e
H representa a tecla Home capacitiva. Uma tecla mapeada acorda a tela de sleep.

Também é possível repetir interações sem controle manual da janela, via
`CROSSPOINT_SIM_INPUT_SCRIPT`, usando ações com tempos em milissegundos desde o
início (`DOWN`, `ENTER`, `HOME`, `TAP:x,y`, `SWIPE:x1,y1,x2,y2`, `QUIT`). A
dependência documenta a sintaxe completa de scripts/screenshot em
`.pio/libdeps/simulator_x4/simulator/README.md`.

## Limitações conhecidas

O host exercita fluxos lógicos de UI/input e renderização SDL; não emula
eletricamente o hardware nem valida GT911 físico, SDMMC, temporização, ghosting,
waveform ou power sequencing do e-ink. O shim host-only
de `MemoryManager` não replica capacidades de heap ESP/PSRAM. O perfil físico do
controlador de display varia entre unidades; SDL não valida essas diferenças.
Operações de sensores e periféricos físicos são simuladas ou indisponíveis.

O fechamento por janela/`QUIT` e uma nova execução iniciam um processo novo; os
dados em `simulator-sd/` permanecem entre execuções. A execução direta do
binário sem o script pode usar o fallback `fs_/` em vez desse cartão.

## Firmware para testar no X4 Pro físico

Na raiz do projeto, execute `bash build-firmware-x4pro.sh`. O script compila
**somente** o perfil `x4pro` e deixa o arquivo instalável em
`firmware-x4pro/firmware.bin`, exibindo o caminho completo e SHA-256 ao final.
Copie esse `.bin` para a **raiz do cartão SD físico** e selecione-o no menu de
atualização de firmware do aparelho. O script não conecta, grava nem reinicia
o X4 Pro. Não use `firmware.factory.bin` para a instalação pelo menu do SD.
