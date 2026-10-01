# Status: Highlights + Notes V1

## Escopo concluído no firmware

- EPUB highlights persistem por livro e usam `{spineIndex, startOffset,
  endOffset}` em codepoints visíveis do spine; o intervalo é meio-aberto.
- A seleção começa por long-press e suporta drag single/multi-word, ida e volta
  na mesma página. Depois de iniciar a seleção, o contato é consumido e não vira
  página. Soltar fora do texto conserva o último endpoint válido.
- Highlights são renderizados novamente após navegar, fechar/reabrir e reiniciar.
  Podem ser removidos no reader.
- O menu do reader (lista clássica e painel More) abre a lista do livro atual.
  Os itens são ordenados por spine, offsets e ID, com uma prévia curta e título
  do TOC quando disponível. Abrir um item volta ao offset, sem depender de um
  número de página persistido. Se a seção atual for parcial e ainda não cobrir o
  offset, o reader continua o build incremental até o alvo.
- Long-press em um item da lista oferece `Open` e `Add note` ou `View / edit
  note`. A nota pertence ao próprio highlight. O teclado existente permite
  digitar, editar, apagar, salvar e cancelar. Nota vazia significa ausência de
  nota; a lista mostra um indicador traduzido quando existe conteúdo.

## Persistência e compatibilidade

- `HighlightEntry` guarda ID, spine, intervalo, quote e nota opcional.
- Sidecar `/.crosspoint/highlights/highlight_<fnv1a64(bookPath)>.bin` usa
  cabeçalho/path, registros e CRC-32 com a escrita recuperável existente via
  `.tmp`/`.bak`.
- Highlight sidecar v2 acrescenta nota UTF-8 limitada a 512 bytes por registro.
  V1 continua legível como nota vazia; a próxima gravação reescreve em v2.
- A seção EPUB usa cache v49 com offsets por palavra; dados v48 são reconstruídos.
- A identidade continua sendo o caminho do arquivo. O formato não detecta uma
  substituição do EPUB feita no mesmo caminho; offsets podem ficar obsoletos nesse
  caso.
- O lookup offset→página percorre a LUT de página existente em O(número de
  páginas), sem índice persistente adicional. A coleção tem limite de 64
  highlights por livro; notas limitam o acréscimo máximo a 32 KiB.

## Arquivos principais

- `docs/highlights-v1-format.md` — sidecar, localização, navegação, limites e
  migração.
- `docs/HIGHLIGHTS_NOTES_PLAN.md` — plano amplo original e fases futuras.
- `src/HighlightEntry.h`, `src/util/HighlightStore.{h,cpp}` — modelo e SD.
- `src/activities/reader/EpubReaderActivity.{h,cpp}` — seleção, renderização,
  menu, navegação e remoção.
- `src/activities/reader/EpubReaderHighlightsActivity.{h,cpp}` — lista,
  indicador de nota, editor e persistência de Notes.
- `src/util/HighlightSelection.h`, `src/util/HighlightNavigation.h`,
  `src/util/HighlightNoteUpdate.h` — lógica pura e atualização com rollback.
- `lib/Epub/Epub/VisibleTextOffsetResolver.h` e `Section.cpp` — página para
  offset visível usando a LUT existente.

## Validação no host

- Host tests: **405/405 passaram** (`cmake --build build/host-tests` e
  `ctest --test-dir build/host-tests --output-on-failure`). Incluem seleção,
  ordenação, resolução de limites de página, roundtrip de Notes, compatibilidade
  HLT v1→v2, UTF-8, limites, cancelamento e rollback.
- `pio run -e simulator_x4`: passou.
- `pio run -e x4pro`: passou; artefato de aplicação em
  `.pio/build/x4pro/firmware.bin`.
- `git diff --check`: passou no fechamento desta fase.
- Smoke isolado no simulator usou um SD temporário: criou 3 highlights pela UI,
  incluindo posições distintas e dois spines; validou lista, ordenação,
  navegação próxima/distante, extensão de uma seção parcial, restart e lista
  vazia. Para Notes, digitou `café`, confirmou o marcador e o texto prefilled
  após restart, cancelou sem alterar, editou para `cafés`, reiniciou e removeu o
  highlight com sua nota.

## Revisão e limites físicos

- Revisão independente estática dos fluxos de store v1/v2, update rollback,
  teclado, lista e ciclo de vida não encontrou defeitos acionáveis. O reviewer
  não executou os testes/builds nem teve o diff completo isolado de toda a
  worktree preexistente.
- `pio run -e default` (C3) permanece bloqueado no ambiente pelo erro externo
  pré-compilação `ModuleNotFoundError: No module named
  'SCons.Tool.FortranCommon'`.
- Não houve flash nem validação física no XTEINK X4 Pro. GT911, dimensões e
  precisão de teclas, latência de digitação, refresh/ghosting, artefatos e
  conforto de edição ainda precisam de teste no aparelho.

## Próximo passo

Aguardar o checklist físico de `PROMPT_08_VALIDACAO_FISICA_HIGHLIGHTS_NOTES.md`
ser executado e corrigir apenas defeitos reproduzíveis. Não iniciar Anki até
fechar a validação física deste marco.
