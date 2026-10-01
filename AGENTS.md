# CrossPoint Reader — guia de desenvolvimento

Este repositório é o firmware CrossPoint para e-readers. Neste workspace, o alvo principal de desenvolvimento é o **XTEINK X4 Pro**; preservar a compatibilidade com os demais dispositivos e preferir mudanças pequenas e integráveis ao upstream. O X4 Pro não é o X4 original: usa ESP32-S3 com 8 MB de PSRAM e 16 MB de flash, display e-ink 800×480, GT911 touch, luz frontal quente/fria e SDMMC. O `default` compila X3/X4 para ESP32-C3 sem PSRAM. Restrições de heap, stack, framebuffer e periféricos devem ser verificadas **por perfil**, não copiadas do C3 para o S3.

## Escopo e fontes de verdade

- Abra o OpenCode na raiz do workspace para descobrir `.opencode/`; execute comandos de firmware neste repositório. Antes de editar, confira `git status --short` e preserve alterações preexistentes. `CrossInk/` e `crossplay/` são referências, não destinos de escrita. Não altere `freeink-sdk/` nem dependências baixadas sem pedido explícito.
- Consulte `platformio.ini` para ambientes/flags, `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h` para perfis/capacidades e `freeink-sdk/docs/xteink-x4pro-support.md` para o hardware. Confirme detalhes nas implementações atuais antes de fixar números ou afirmar ganhos de memória/desempenho.
- Pontos de entrada: `src/main.cpp`, `src/activities/ActivityManager.*`, `src/activities/reader/`, `lib/Epub/`, `lib/hal/`, `src/MappedInputManager.*`, `src/components/UiAppHost.*` e `src/activities/UiListActivity.*`.
- Para persistência, veja `lib/Serialization/`, `src/*Store.*`, `src/util/Bookmark*`, `src/activities/reader/ProgressFile.h` e `docs/file-formats.md`. Antes de definir um formato novo para highlights, notas ou Anki, confira os contratos existentes e esclareça requisitos do produto não definidos no código.

## Build e verificação

- `pio run -e x4pro` compila o X4 Pro; `pio run -e default` compila o binário C3 para X3/X4. Outros perfis, inclusive `x4c`, `sticky` e `papermono`, estão em `platformio.ini`. O `default` de PlatformIO é o C3 (`default_envs = default`): não use `pio run` sem `-e` como evidência de validação do X4 Pro.
- Os perfis X4 Pro usam a variante S3 com PSRAM e `USE_BLOCK_DEVICE_INTERFACE=1` para SDMMC; estendem `base`, não `firmware_tuned`, para manter o TinyUSB do core precompilado necessário ao USB-MSC. Não transplante ajustes `firmware_tuned_c3` para o Pro.
- O firmware usa `-std=gnu++2a` e `-fno-exceptions` em `[base]`; verifique flags concretas no perfil antes de depender delas. Para logs, use as macros de `Logging.h`. Para formatação C/C++, use `./bin/clang-format-fix -g` em vez de invocar `clang-format` diretamente.
- Testes de host estão em `test/CMakeLists.txt` (CMake/CTest). O simulador SDL2 neste checkout está em desenvolvimento (`platformio.sim.ini`, `scripts/sim_catchup.py`); não trate execução no host como teste de touch, consumo, SDMMC ou refresh e-ink reais.
- Faça a verificação adequada ao que mudou: documentação não exige recompilar; código de firmware pede build do perfil afetado e testes de host relevantes. Descreva o que foi verificado e o que ainda requer aparelho físico. Não faça upload/flash sem pedido explícito.

## Arquitetura e recursos

- Use `lib/hal/` para display, GPIO e armazenamento; não acesse `InputManager`, `SDCardManager` ou `FsFile` diretamente de atividades. `HalStorage` serializa I/O com mutex; use `Storage` e `HalFile` para SD. Feche arquivos explicitamente antes de remover/renomear/reabrir o mesmo caminho ou ao liberar um handle de longa duração. Arquivos locais se fecham ao sair do escopo.
- `ActivityManager` possui `currentActivity`, `pendingActivity` e a pilha via `std::unique_ptr<Activity>` (`src/activities/ActivityManager.h`). Navegações são agendadas e processadas em `loop()`: `replaceActivity()` descarta a pilha, `pushActivity()` preserva a tela anterior e `popActivity()` a retoma. A saída chama `onExit()` antes de destruir a atividade; limpe tarefas e recursos que possam usar a atividade antes da destruição. Respeite `RenderLock` e não espere uma renderização enquanto o mantém.
- Evite alocar repetidamente em caminhos de renderização. Reutilize buffers e linhas de UI quando apropriado; escolha alocações segundo as capacidades do perfil e trate falhas onde a operação precisar ser recuperável. `makeUniqueNoThrow` está em `lib/Memory/Memory.h`; `std::make_unique` também aparece no código, então não presuma que toda alocação já seja tratada como falível.
- Use `std::string_view` somente com o comprimento explícito; `.data()` não garante terminação NUL para APIs C. Em ISR, use APIs `FromISR` e verifique os requisitos de IRAM/DRAM no alvo. Evite casts que assumam alinhamento em dados binários; use leitura/serialização segura.
- Textos exibidos ao usuário devem usar `tr(STR_*)`; as fontes são traduzidas em `lib/I18n/translations/*.yaml`. Evite strings de interface embutidas, inclusive em diálogos de erro. Mensagens de log podem ser literais.

## UI e entrada

- Para uma tela nova use FreeInkUI pelo host do firmware: `UiListActivity` para listas, `UiTabListActivity` para listas com abas, `UiAppHost` para layouts próprios. Veja `docs/contributing/touch-and-ui.md`. O home e a página do reader têm superfícies legadas; não replique seus testes manuais de coordenadas em telas novas.
- Acesse botões lógicos via `MappedInputManager::Button` e touch pelo fluxo compartilhado de UI; respeite capacidades do perfil (`hasTouch()` etc.). Home/reader tratam gestos específicos. Não leia pinos diretamente nem assuma quatro botões frontais no X4 Pro, que tem dois botões de navegação digitais e Home capacitivo.
- Use `UITheme`/tokens para chrome e métricas e `GfxRenderer` para dimensões e margens orientadas (`getScreenWidth()`, `getScreenHeight()`, `getOrientedViewableTRBL()`). Verifique orientação e áreas de toque nas telas relevantes.

## SD, progresso e caches

- Dados do firmware ficam no SD sob `/.crosspoint/`. O cache EPUB é `/.crosspoint/epub_<hash-do-caminho>/` (`lib/Epub/Epub.h`); contém dados **recriáveis** como `book.bin` e `sections/*.bin`, mas também `progress.bin`. Renomear/mover o livro muda a chave de caminho e pode deixar o progresso anterior no diretório antigo.
- `book.bin` está na versão **10** (`lib/Epub/Epub/BookMetadataCache.cpp`); os arquivos de seção estão na versão **48** (`lib/Epub/Epub/Section.cpp`). Compare sempre as constantes do código e `docs/file-formats.md` antes de modificar layout ou lógica de invalidação. Uma versão diferente é rejeitada/reconstruída; mudanças de layout sem alteração binária também podem exigir bump.
- Não apague `/.crosspoint/` nem a pasta inteira de um livro como primeira resposta a problema de renderização: isso pode eliminar progresso e outros dados. Para diagnosticar seção, identifique e remova apenas os artefatos recriáveis pertinentes, com cuidado para preservar `progress.bin`; verifique como a rotina de cache do livro e as configurações de layout atuam antes de recomendar invalidação.
- Ao alterar persistência, evite escritas redundantes no SD e preserve os contratos de leitura/versão. `ProgressFile::writeAtomic` grava primeiro `progress.bin.tmp` e depois substitui o arquivo canônico; não troque por truncamento direto.

## Arquivos gerados e Git

- Edite fontes HTML em `data/html/`, não `src/network/html/*.generated.h`. Para traduções, edite `lib/I18n/translations/*.yaml`, não os arquivos gerados `lib/I18n/I18nKeys.h`, `I18nStrings.h` e `I18nStrings.cpp`. Os scripts `scripts/build_html.py` e `scripts/gen_i18n.py` são acionados no build via `platformio.ini`.
- `platformio.local.ini` é para ajustes locais e não deve ser commitado. `platformio.sim.ini` pode conter trabalho local experimental neste checkout; confira o status antes de tocá-lo. Não inclua `.pio/`, `build/` ou outros artefatos.
- Confira branch, remotes, status e diff antes de operações Git. Neste workspace `origin` é o fork pessoal e `upstream` é o oficial; confirme a base da integração em vez de assumir `develop` ou qualquer outra branch. Não faça commit, push, PR ou flash sem pedido explícito. Nunca adicione autoatribuição de assistente como coautor.
