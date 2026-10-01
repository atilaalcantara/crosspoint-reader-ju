# Plano técnico: highlights e notas no reader EPUB

## Escopo e conclusão

Este documento planeja a primeira versão de seleção, highlights e notas no
reader EPUB do X4 Pro. Não implementa funcionalidades. O reader, os formatos de
cache e as interfaces descritos abaixo foram conferidos no estado atual de
`crosspoint-reader-ju/`; CrossInk é uma referência de comportamento, não a base
de port.

**Direção recomendada:** manter parsing e paginação no `lib/Epub`, criar uma
atividade de seleção de intervalo que consuma as palavras e caixas da `Page`
atual, e persistir uma entidade própria `Highlight` por livro fora do cache de
layout. Identificar o trecho por spine + intervalo de offsets de codepoints
visíveis do corpo do spine; o texto selecionado é uma confirmação e fallback,
não a única âncora. Desenhar o highlight em cada render resolvendo o intervalo
contra os offsets por palavra. Associar a nota opcional ao mesmo registro do
highlight e editar por `KeyboardEntryActivity`.

O principal trabalho de base é propagar, por palavra renderizada, a faixa de
offsets que `ParsedText` já calcula e disponibilizá-la no cache `TextBlock`.
Essa mudança precisa ser versionada e medida em memória/builds C3 e S3; offsets
de página atuais não identificam uma palavra dentro da página.

## A. Estado atual do projeto principal

### Abertura, layout e páginas

- `src/activities/reader/ReaderActivity.cpp::create` despacha o tipo de arquivo;
  EPUB chega a `EpubReaderActivity` (`src/activities/reader/ReaderActivity.cpp`).
- `src/activities/reader/EpubReaderActivity.cpp::loadBook` abre o `Epub`,
  configura cache e carrega `progress.bin`. O cache EPUB usa
  `/.crosspoint/epub_<std::hash(path)>` (`lib/Epub/Epub.h:21-26,43-46`).
- `EpubReaderActivity::renderBook` seleciona o spine, monta o
  `ReaderRenderSpec`, cria/carrega `Section` e solicita `Section::loadPage`;
  `Page::render` é chamado em `renderContents` (`EpubReaderActivity.cpp`, em
  especial `1146-1208,1410-1449`). `currentSpineIndex` + `Section::currentPage`
  nomeiam a posição visual corrente; a página pertence ao spine EPUB, não é uma
  localização textual estável.
- `Section` constrói páginas incrementalmente e persiste page LUT, anchor map,
  paragraph/list-item LUT e visible-text LUT (`lib/Epub/Epub/Section.h:26-75,
  117-168`). A versão atual de section é **48** (`Section.cpp:57`); o formato é
  invalidado/reconstruído quando a versão ou parâmetros do layout não batem.
  `docs/file-formats.md:91-100,126-132,164-185` descreve a evolução e conteúdo.
- `Page` contém `PageLine`, `TextBlock`, imagem e regra horizontal
  (`lib/Epub/Epub/Page.h:16-90`). `PageLine` dá posição x/y e acesso ao bloco;
  `TextBlock` expõe texto, comprimento, x, estilo e ordem por palavra
  (`TextBlock.h:80-111`). Portanto as caixas desenhadas podem ser recuperadas
  com as métricas da fonte/linha, mas os offsets fonte da palavra não estão no
  objeto/cache atual.

### Texto e âncoras existentes

- `ParsedText` mantém texto tokenizado e um mapa temporário de offsets visíveis
  (`lib/Epub/Epub/ParsedText.h:17-60`). Esses valores são offsets de
  **codepoints Unicode visíveis no corpo do spine**, não bytes UTF-8 (`:49-52`).
  A implementação calcula offsets por palavra, inclusive quando divide,
  reordena ou hifeniza (`ParsedText.cpp:376-415,444-476,528-575,1274,1370`).
- `Section::PageLutEntry` e `Page::visibleTextOffset` guardam somente o início
  da página, e `getPageForVisibleTextOffset()` resolve offset para página
  (`Section.h:26-33,151-168`; `Section.cpp:1018-1104`). Isso permite sobreviver
  a reflow para posição aproximada, não marcar um intervalo dentro da página.
- `TextBlock` armazena palavras e coordenadas/layout num arena compacto
  (`TextBlock.h:14-39,50-70`), sem offset fonte por palavra. Anexar intervalos
  ali exige decidir a representação e atualizar serialização/cache; a opção
  direta `uint32_t start/end` por palavra custa memória/SD, logo a fase deve
  medir o custo no perfil C3 e considerar compressão por base/deltas já usada
  em `ParsedText` antes de fixar o layout.
- `Section` também resolve fragmentos HTML, parágrafo XPath e item de lista
  (`Section.h:123-149`). São úteis como fallback/retorno, mas não substituem
  intervalo textual: um parágrafo pode ocupar páginas, e página/word ordinal
  dependem do layout.

### Input e interação

- `EpubReaderActivity::loop` dá prioridade a links tocados, menu central,
  navegação touch/page-turn e atalhos; long press do botão Confirm atualmente
  aciona ação configurável (bookmark, dicionário, sync, menu), não seleção de
  trecho (`EpubReaderActivity.cpp:530-619`; gestos comuns em
  `src/activities/reader/ReaderUtils.h`). A nova ação deve ser explícita e não
  roubar gestos de virar página nem alterar shortcuts sem uma decisão de UX.
- `MappedInputManager` oferece eventos lógicos de touch/tap/held/release; o
  X4 Pro tem touch GT911, Home capacitivo e dois botões laterais. O host
  `simulator_x4` seleciona o perfil Pro e recebe mouse como tap/swipe, conforme
  a validação anterior. A coordenada do toque deve ser convertida para a mesma
  área lógica que `Page` usa, descontando margens orientadas.
- `DictionaryWordSelectActivity` é a referência local mais próxima para
  selecionar palavra por coordenada: `extractWords()` percorre PageLine e
  `TextBlock`, cria `WordBox` com x/y/estilo/texto (`.cpp:62-110`),
  `wordAt()` faz hit test com slop (`:113-125`), e touch-down/tap move ou
  confirma palavra (`:252-270`). Ela cobre palavra única para dicionário, não
  intervalo persistente nem drag para seleção.
- `EpubReaderFootnoteSelectActivity` e `EpubReaderPercentSelectionActivity`
  demonstram subatividade/overlay com toque no contexto reader. `Activity` e
  `startActivityForResult()`/`ActivityResult` são o fluxo existente para
  atividades filhas (`src/activities/Activity.h:16-64`; `ActivityResult.h`).
  Uma atividade de seleção pode retornar resultado tipado e deixar o reader
  dono de persistência e lifecycle.

### Progresso, bookmarks e caches

- `progress.bin` mantém spine/page/pageCount e opcionalmente o offset de página
  visível em 4 bytes adicionais (`EpubReaderUtils.h:14-40`; leitura em
  `EpubReaderActivity.cpp:209-228`). `ProgressFile::writeAtomic()` grava `.tmp`,
  fecha, remove o destino e renomeia (`ProgressFile.h:12-61`); é crash-safe
  contra arquivo canônico rasgado, mas no FAT há uma janela sem arquivo e não
  é atomicidade completa.
- Bookmark já oferece um exemplo de dado persistente separado do cache:
  `BookmarkEntry` tem XPath/summary/nome/percentual/spine/page e offset visível
  (`src/BookmarkEntry.h:6-24`), `BookmarkFile` serializa JSON
  (`src/util/BookmarkFile.cpp:10-77`) e a tela consulta/navega/renomeia/exclui.
  O caminho é derivado do filepath (`BookmarkUtil.cpp:6-19`). Rename de EPUB
  move cache e arquivo bookmark junto de forma coordenada (`FileBrowserActivity.cpp:480-496`).
- `PersistableStoreBase::writeDocToFile()` serializa JSON, mas delega gravação
  direta a `Storage.writeFile()` (`lib/Serialization/PersistableStore.cpp:10-19`);
  não oferece troca `.tmp`/backup. Portanto sua conveniência não é, sozinha,
  garantia adequada para dados que o usuário não pode perder.
- `BookMetadataCache` está em versão 10, `Section` em versão 48
  (`BookMetadataCache.cpp:14`, `Section.cpp:57`; `docs/file-formats.md`). Cache
  de página/layout é recriável e pode ser apagado; highlights/notas não devem
  residir nele nem ser apagados com `clearCache()`.
- A identidade local atualmente disponível é o caminho do arquivo; não foi
  encontrado UUID estável compartilhado entre EPUB, biblioteca e stores. O
  rename existente é explícito e move estados vinculados. Substituir um EPUB
  mantendo o mesmo nome/caminho requer detecção de alteração do conteúdo.

### UI e entrada de nota

`KeyboardEntryActivity` já oferece teclado FreeInkUI, campo editável, suporte a
toque, cursor UTF-8 e resultado `KeyboardResult` (`src/activities/util/KeyboardEntryActivity.h:13-29,45-101`).
É adequado para uma nota simples sem teclado novo. As chamadas existentes em
`SettingsActivity`, `OpdsSettingsActivity` e `EpubReaderBookmarksActivity`
demonstram abertura por `startActivityForResult`. Usar title traduzido,
limite explícito de bytes/codepoints e retornar/cancelar sem salvar exige
integração de produto, não mudança do teclado em princípio.

## B. CrossInk: seleção e clippings existentes

O CrossInk foi inspecionado em checkout limpo no momento da análise. A
implementação é maior do que uma atividade isolada e não inclui Notes como
parte do clipping consultado.

- `src/activities/reader/EpubReaderActivity.cpp` prepara as palavras da página,
  cria o `ClipWordStore`, guarda contexto de livro/spine/layout e abre
  `ClipSelectionActivity` com `startActivityForResult`; na volta adiciona ao
  `CLIPPINGS` e exporta `/My Clippings.txt` (código da integração de gravação
  em `:4547-4599`). List/detail e drawing são outras integrações no reader.
- `src/activities/reader/WordRef.h` guarda `x/y/w/h`, page index, ordinal da
  palavra na página, estilo, flags RTL/hífen/parágrafo e offsets no pool de
  texto (`:11-25`). `ClipWordStore` limita o texto a `UINT16_MAX` bytes
  (`:29-53`). Esses índices locais descrevem layout/página em memória.
- `ClipSelectionActivity` (`.h:27-98`, `.cpp`) monta ordem de leitura RTL/LTR,
  limita seleção a 240 palavras (`.cpp:175-209`, `.h:43-45`), começa em palavra
  do meio da página e usa toque em palavra para mover/confirmar; long press de
  500 ms inicia drag. Release finaliza; botões navegam e Confirm marca/termina
  (`.cpp:244-355`). Coordenadas selecionam `WordRef` por retângulo
  (`.cpp:358-372`); seleção pode atravessar páginas com hold na borda
  (`.cpp:267-289`).
- O highlight ativo é repintado por palavra, preenchendo pixels alternados da
  framebuffer e contornando o cursor (`.cpp:561-619`); para mudar de página
  recarrega `Page`/cache e preserva snapshots de framebuffer com fallback
  (`.cpp:487-558`). Esse manejo de buffer/fontes/locks deve ser estudado, não
  copiado às cegas.
- `ClipTextBuilder::SelectionBounds` expressa fragmento de palavra por
  page-index, ordinal e offsets **em bytes UTF-8 dentro da palavra**
  (`src/clippings/ClipTextBuilder.h:11-22`). Resultado contém página/word
  ordinals, texto e metadados do clipping. `Clipping` em `ClippingStore.h:29-47`
  persiste spine, páginas, índices/contagem de palavras, parágrafo, timestamp,
  assinatura de layout, seleção de tabela e localização do texto; não é âncora
  imune a reflow.
- `ClippingStore` aceita EPUB, grava por livro sob
  `/.crosspoint/clippings/epub_<CRC32(filepath)>.bin`
  (`ClippingStore.cpp:16-36,93-115`). O cabeçalho guarda título/autor/path; há
  até 256 clippings por livro e 4 KiB de texto por clipping
  (`ClippingStore.h:8-19`). As versões 1–4 são lidas; v2 introduz offsets de
  texto, v3 assinatura de layout, v4 seleção de tabela (`.cpp:17-20,271-357`).
- A escrita gera `.tmp`, sincroniza, fecha, renomeia o destino para `.bak`,
  renomeia o temporário ao nome final e tenta rollback/recuperação (`.cpp:360-484`).
  A leitura valida versão, limite de quantidade, comprimento e truncamento
  (`.cpp:264-357`). Arquivo é identificado por filepath; `BookMoveUtils.cpp`
  migra no rename (`:74-108,173-230`).
- `EpubReaderClippingListActivity` lista, abre detalhe, exclui e retorna
  `spineIndex/startPage/pageCount/paragraphIndex` ao reader
  (`.cpp:210-267`). Reader resolve e renderiza destaques registrados; a
  assinatura de layout e o texto permitem buscar/reconstruir faixa para caches
  incompatíveis, mas não eliminam ambiguidades por texto repetido. A lista
  global fica em `SavedItemsHomeActivity`.
- `ClippingsManager` escreve texto exportado em `/My Clippings.txt`
  (`src/clippings/ClippingsManager.h:7-10`; `.cpp:113-153`). Essa exportação é
  paralela ao armazenamento binário interno. Não é necessária para V1.
- Não encontrei um campo ou fluxo de edição de nota associado ao clipping. A
  lista/detalhe mostra e exporta o texto selecionado, não uma nota distinta.

## C. Gap analysis e reaproveitamento

| Conceito CrossInk | Estado no principal | Tratamento recomendado |
|---|---|---|
| Coordenada → palavra/caixa | Já existe base em `DictionaryWordSelectActivity`; usa `PageLine/TextBlock` atual | Reutilizar o padrão de hit test/margens; extrair helper somente se houver dois consumidores claros. |
| Ordem visual LTR/RTL | `TextBlock` tem estilo/posição; seleção de dicionário não cria leitura por faixa | Adaptar inversão de ordem RTL de CrossInk; validar ruby, hifenização e palavras quebradas. |
| Long-press + drag de seleção | Reader tem gestos de page-turn/menu e seletores pontuais, não intervalo de texto | Reimplementar como atividade filha, com prioridade/input gate explícitos para não conflitar com gestures do reader. |
| WordRef/ClipWordStore | Não existe estrutura equivalente persistente no principal | Não copiar pool inteiro sem razão; Page/TextBlock já contém palavra e posição. Criar snapshot temporário compacto da página/faixa necessária. |
| Highlights pixel/snapshot | `DictionaryWordSelectActivity` já tem highlight temporário e repaint diferencial | Reaproveitar técnica de framebuffer apenas na seleção transitória; destaque persistente deve derivar do modelo e ser redesenhado no render normal. |
| Store por livro/versionado | Bookmark e ProgressFile são os precedentes no principal | Criar `HighlightStore` próprio, não usar `progress.bin` nem cache `Section`. Adotar limites, leitura validada e estratégia de escrita robusta. |
| Lista/detail/return/delete | Atividades de bookmark já oferecem lista, menu, confirmação e retorno por `ProgressChangeResult` | Adaptar componentes e lifecycle; não portar tela específica. Uma lista por livro é suficiente inicialmente. |
| Rename migration | FileBrowser já move cache/bookmark com rollback | Integrar arquivo de highlights ao fluxo de rename de EPUB e adicionar rollback equivalente. |
| `/My Clippings.txt` | Nenhuma exigência de export no V1 | Deixar fora. |
| Notes | Não há em CrossInk no código de clipping inspecionado | Projetar como campo no registro próprio de Highlight; usar teclado atual. |

**Pacote conceitual aproveitável:** fluxo de seleção por word boxes, ordenação
RTL, estado de dois endpoints, regras de touch/drag/confirm/cancel, ideia de
resultado tipado e limite explícito de recursos. `ClipSelectionActivity` e
`ClippingStore` dependem de APIs/estado próprios daquele reader e arquivos
`FsFile`; devem ser adaptados para `Page`, `Section`, `HalFile/Storage`,
`ActivityResult` e lifecycle do fork. Copiar todos os arquivos traria stores,
formatos, exportação, migração e código de cache/repaint que não são necessários
para Notes V1.

## D. Arquitetura proposta

1. **Modelo independente de UI:** `Highlight` com id estável, identidade do
   livro, spine, intervalo textual `[startOffset, endOffset)`, texto citado,
   metadados leves (título de capítulo/timestamp/layout fingerprint) e nota
   opcional. `HighlightStore` coordena load/add/update/remove por livro.
2. **Metadados de localização no EPUB:** propagar faixa de offset visível do
   token no estágio `ParsedText` para palavra no `TextBlock` da `Page`, com
   serialization/deserialize. Medir representação comprimida/memória e bump de
   `SECTION_FILE_VERSION` de 48 para próxima versão; documentar em
   `docs/file-formats.md`. Rebuild é cache-only; não altera a fonte do EPUB.
3. **Seleção filha do reader:** ao entrar, carregar as páginas necessárias e
   construir snapshot transitório de palavras em reading-order, contendo
   retângulo, texto e offsets. Long press no alvo inicia seleção; drag move
   endpoint, toque pode confirmar/ajustar, botões navegam/ajustam, Confirm
   finaliza, Back cancela ou limpa endpoint. Entregar intervalos lógicos via
   `ActivityResult`; atividade não grava SD diretamente.
4. **Persistência e retorno no reader:** reader recebe resultado, cria registro
   no store, confirma gravação antes de toast e renderiza de novo. Store fora do
   cache `epub_<hash>` para `clearCache()`/mudança de fonte não apagar dados.
5. **Render normal:** para palavras da página em tela, comparar intervalos
   visíveis com highlights carregados do spine; agrupar palavras contíguas por
   faixa e pintar por cima/na rotina da página respeitando margens, night mode,
   orientação, refresh e links. Sem efeito colateral da seleção temporária.
6. **Consulta:** abrir lista de highlights do livro por item do menu reader;
   item mostra trecho + capítulo/nota indicator, detalhe mostra texto e nota;
   ações para navegar, editar nota e remover.
7. **Notes:** abrir `KeyboardEntryActivity` com texto atual/inicial vazio; só
   atualizar o mesmo highlight quando resultado não cancelado e store confirmar
   save. Não criar dependência de Notes em parsing/UI de EPUB.

Não colocar estado de coleção em `APP_STATE`, que é contexto/posição atual, nem
em `CrossPointSettings`, que é preferência. Não reutilizar bookmark como
highlight: bookmark é posição única, não contém intervalos nem múltiplos dados
de texto/notas.

## E. Persistência e âncora recomendadas

### Contrato conceitual

- **Identidade local:** filepath canônico normalizado do EPUB, igual ao
  ecossistema atual de cache/bookmarks/biblioteca. Manter cabeçalho com path e
  usar nome flat derivado/hash para o arquivo de store. Integrar rename no
  `FileBrowserActivity` com rollback; não deixar o novo sidecar se separar do
  EPUB após rename.
- **Detectar substituição do conteúdo:** gravar fingerprint leve do arquivo
  original (tamanho + modification time reportado por `HalFile`, além de
  metadados spine/EPUB quando disponíveis). Se divergir, não desenhar
  silenciosamente âncoras antigas no conteúdo novo: manter registros/notas,
  sinalizar localização não validada e não fazer jump automático até revalidar
  texto/âncora. Tamanho + mtime não são hash criptográfico; replacement que
  preserve ambos pode passar despercebido. Essa limitação fica explícita até se
  justificar digest completo do EPUB.
- **Âncora:** `{spineIndex, startVisibleCodepoint, endVisibleCodepoint}` com
  intervalo meio-aberto e `<body>` do spine como domínio, na unidade já usada
  pelo parser (`ParsedText.h:49-52`). Manter trecho UTF-8 selecionado para
  confirmação e fallback de recuperação. Timestamps são informação auxiliar,
  não identidade. `chapterTitle` é rótulo mutável, não chave.
- **Reflow:** com offsets por palavra no `TextBlock`, reflow muda geometria/page,
  não âncora; resolve intervalo novamente sobre palavras visíveis e página
  corrente. Fallback textual só pode sugerir recuperação e exige correspondência
  única validada no spine. Offset sozinho não deve ser aplicado após mudança de
  fingerprint do documento.
- **Alterações em EPUB:** se a fonte mudou, conservar o registro para não perder
  notas; marcar âncora como stale/órfã e não pintar/jumpar até resolver. Não
  migrar offsets automaticamente entre conteúdos diferentes nem remover dados.
- **Escrita:** guardar coleção num sidecar separado sob `/.crosspoint/` por
  livro. Formato inicial pode ser JSON versionado se for bounded e validado, mas
  não chamar `PersistableStoreBase::writeDocToFile()` como se fosse atômico: ele
  usa `Storage.writeFile()` direto. Implementar escrita staging + flush/close +
  replace com backup/rollback e leitura que rejeita truncamento, versões
  desconhecidas, limites estourados e strings inválidas. Tratar ausência como
  coleção vazia; corrupção deve dar erro recuperável, nunca apagar arquivo
  silenciosamente.
- **Versionamento:** envelope `version` explícito e records opcionais; aceitar
  ausência de `note` como string vazia nos dados v1. Versionar migrações de
  sidecar independentemente de `book.bin`/`section.bin`. Definir limites
  explícitos de quantidade, citação e nota para heap interno e tamanho de
  escrita; validar UTF-8 e não truncar no meio de codepoint.
- **Limite de atomicidade:** FAT não fornece transação de diretório. Use `.tmp`
  e `.bak` para recuperar a última coleção íntegra; a rotina deve definir e
  testar o que fazer se falhar entre cada rename. Fechar `HalFile` antes de
  remover/renomear conforme contrato de `HalStorage`.

### Notas no registro

Guardar `note` opcional **no mesmo registro** do highlight, não num arquivo de
notes separado. A relação é um-para-um/zero-para-um e não existe lifecycle de
nota independente; manter juntas torna editar e remover a entidade uma gravação
coerente e evita foreign keys/orphans e transação cross-file. Reservar `id`
estável mesmo que V1 inicialmente use ordenação por criação; lista e callback
não devem identificar registro por índice vetorial após remoções. Notes antigas
sem campo carregam como vazio. Edição salva record inteiro por staging; cancel
não modifica store.

## F. Fluxo de Notes

1. Depois de salvar um highlight, mostrar ação curta “Adicionar nota” e “Agora
   não”; alternativamente a lista dá “Editar nota”. Não tornar teclado
   obrigatório no fluxo de seleção.
2. Abrir teclado touch reutilizável com título traduzido, string inicial,
   comprimento máximo definido e cursor UTF-8. Focar área textual, oferecer
   Confirm/Cancel do componente já existente.
3. No OK, validar string, atualizar highlight pelo `id`, persistir e só então
   refletir o sucesso; em falha preservar a nota anterior e informar erro. Em
   cancel, retornar sem gravar.
4. Detalhe da lista apresenta nota read-only e botão Edit; highlight e nota
   seguem juntos ao navegar/remove.

## G. UX V1 proposta

- Abrir EPUB → iniciar seleção por toque longo numa palavra (hit-target com
  margem de dedo) ou ação reader explícita. A escolha entre gesto e ação deve
  evitar conflito com long-press existente de dicionário/bookmark e é decisão
  de produto para confirmar antes da implementação.
- O texto fica visível; início tem preenchimento inverso e cursor com borda.
  Arrastar ajusta fim; pressionar/soltar outra palavra ou botões oferece caminho
  sem drag. Reader mostra instruções curtas “arraste; confirmar salva; voltar
  cancela”. Seleção em várias páginas muda página sob o dedo com dwell de borda,
  mantendo indicação de início/fim.
- Confirmar salva. Mostrar destaque com inversão/texture e uma marca consistente
  em toda página; e-ink deve permitir leitura de caracteres sob o destaque e
  preservar atualização parcial apropriada. Depois da confirmação, oferecer
  nota opcional.
- A lista do livro usa toque/lista `UiListActivity` e mostra trecho curto,
  capítulo e marcador de nota. Abrir item: detalhe, “Ir ao texto”, “Editar
  nota”, “Remover”. Remoção pede confirmação simples. O retorno vai à página
  resolvida pelo offset, com scroll/página sob o trecho.
- Não oferecer cores/categorias/export/sync na V1.

## H. Estratégia de validação

### Host (`simulator_x4`)

- Fixture EPUB determinística com texto curto, duas páginas e dois spines;
  incluir LTR, RTL, palavras repetidas, acentos/case, hífen no fim da linha,
  parágrafo quebrado e texto próximo a link. Abrir o reader e validar seleção
  e caixa das palavras em orientação/margens atuais.
- Entrada: toque longo começa, drag muda endpoint, ajuste em reverso, tap/Confirm
  termina, Back cancela e limpa endpoint; swipe de page-turn e links continuam
  funcionais fora da seleção.
- Render: texto destacado após navegar para fora/voltar, atualização após
  repaginar por fonte/orientação, sem seleção sobre texto/imagem ou caixa
  incorreta em RTL; highlights múltiplos sobre a mesma página.
- Persistência: adicionar, restart do processo simulador, reabrir EPUB, validar
  highlight e nota; editar, cancelar, remover; reabrir e confirmar consistência.
  Criar sidecar parcial/versão desconhecida em fixture isolada para verificar
  erro/recuperação sem destruir backup; exercitar limite/corrupção com testes
  host do store, não por arquivos do cartão real.
- Navegação: lista por livro, abrir item retorna a spine/página contendo início;
  testar texto repetido e offset dentro/fora de página.
- Input SDL do host suporta mouse como tap/swipe e script temporal, bom para
  smoke/regressão; validar também taps em posições reproduzíveis. Reproduzir
  long press/drag depende de eventos SDL/driver de touch do simulator: confirmar
  que o script/SDL de fato emite duração/held e movimento como o `MappedInput`
  consome antes de alegar cobertura automatizada completa.

### Aparelho físico

No X4 Pro validar pressão/arraste reais e orientação/calibração GT911, precisão
da seleção com dedo, alvo de teclado, legibilidade/ghosting e atualização e-ink
dos destaques, latência/flash, tamanho de heap interno e PSRAM, leitura/gravação
em SDMMC e interrupção de energia durante staging/rename. O simulador não modela
calibração física, falhas de alimentação/FAT, waveform, ghosting nem latência
real. Build mínimo após implementação: `pio run -e simulator_x4`, testes CMake
de host pertinentes e `pio run -e x4pro`; se código compartilhado de page/cache
afetar C3, também `pio run -e default`.

## I. Riscos e decisões abertas

1. **Entrada da seleção:** long press versus ação/menu. Os shortcuts existentes
   são configuráveis e o reader já usa gestos; escolher sem teste de UX pode
   roubar dicionário/bookmark/page-turn. Recomendo ação explícita do menu como
   caminho confiável e long press somente se puder ser configurado sem conflito.
2. **Custo por palavra no cache:** offsets persistentes por token podem aumentar
   seção/heap; antes de escolher campos, medir em fixture EPUB grande no C3 e
   confirmar margem S3. Investigar codificação por base/deltas/varint e
   recuperação via fonte quando cache não tem offsets.
3. **Fingerprint do EPUB:** tamanho + modification time é leve mas não prova
   identidade; decidir se isso basta com quote-validation ou se digest completo
   é necessário para diferenciar replacement com metadados preservados.
4. **Normalização/intervalo:** parser expõe offsets de codepoints visíveis; é
   preciso fixar inclusões de whitespace, entidades HTML, ruby, ligatures e
   token de palavra partida para intervalos [start,end) baterem no texto citado.
   Provar com testes fixture antes de fechar o schema.

Decisões de backup, store fora do cache, note no mesmo record, e âncora textual
foram resolvidas acima. Não há fundamento no código atual para afirmar identidade
canônica global do EPUB nem persistência exata de palavras; essas são as
incertezas técnicas reais.

## J. Fases incrementais para implementação futura

### Fase 1 — metadados de palavra e fixtures

- **Objetivo:** transportar offsets source do parser às palavras cacheadas e
  expor intervalos da página sem UI nem escrita de highlights.
- **Áreas:** `ParsedText`, `TextBlock` serialization/deserialization,
  `ChapterHtmlSlimParser`, `Page`/`Section`, `docs/file-formats.md`, testes de
  parser/cache.
- **Dependências:** decidir intervalo de codepoints/body e codificação após
  medir custo de memória no `default` C3 e `x4pro`.
- **Aceitação:** offsets monotônicos por ordem de fonte para fixture LTR/RTL,
  acertam texto UTF-8/entidades/palavras partidas; reflow conserva âncora;
  cache antigo é rejeitado/reconstruído por novo version byte; build C3/S3.

### Fase 2 — seleção transitória no reader

- **Objetivo:** selecionar e cancelar range em texto visível, sem persistir.
- **Áreas:** nova `EpubReaderTextSelectionActivity` (ou nome equivalente),
  resultado tipado em `ActivityResult.h`, integração controlada em
  `EpubReaderActivity`, helper/pattern de `DictionaryWordSelectActivity`.
- **Dependências:** Fase 1 e decisão do ponto de entrada/precedência de gestures.
- **Aceitação:** iniciar endpoint, arrastar/ajustar, LTR/RTL e borda de página,
  Confirm retorna `[start,end)`, Back cancela restaura posição reader; links,
  page-turn, menu e dicionário não regressam. Sem escrita SD.

### Fase 3 — store por livro e lifecycle

- **Objetivo:** salvar/carregar/listar/remover highlight textual sem render
  persistente ainda.
- **Áreas:** `src/HighlightEntry.h`, `src/util/HighlightFile.*`/`HighlightStore.*`,
  identidade do livro e rename em `FileBrowserActivity.cpp`, testes host de
  serialização/failure path.
- **Dependências:** schema v1, fingerprint e limites decididos; usar
  `HalStorage/HalFile`, não APIs raw em atividade.
- **Aceitação:** multiple records com IDs, roundtrip, truncamento/version
  desconhecida não sobrescrevem dados, staging/backup recupera última versão,
  rename move junto com rollback, `clearCache()` deixa sidecar intacto.

### Fase 4 — destaque persistente e navegação de volta

- **Objetivo:** renderizar intervalos carregados e navegar do item à âncora.
- **Áreas:** `EpubReaderActivity::renderBook/renderContents`, desenho
  compartilhado de `PageLine/TextBlock`, lookup em `Section`, store e lista
  `UiListActivity`.
- **Dependências:** Fases 1–3, estratégia de refresh/inversão.
- **Aceitação:** destaque permanece após page-turn, reflow, close/reopen e
  restart; lista leva ao trecho exato mesmo quando página mudou; remoção elimina
  destaque; limites de fonte/layout/overlap passam host tests.

### Fase 5 — nota em highlight

- **Objetivo:** adicionar/ver/editar nota textual persistida.
- **Áreas:** campo opcional em schema/store, tela de detalhe/ações, chamada
  existente de `KeyboardEntryActivity`, strings de tradução.
- **Dependências:** Fase 3 e limites de nota definidos.
- **Aceitação:** highlight antigo sem nota lê vazio; add/edit persiste após
  restart; Cancel deixa dado inalterado; remoção remove nota junto; erro de SD
  mantém conteúdo anterior e mostra falha; teclado opera touch X4 Pro.

### Fase 6 — endurecimento em dispositivo

- **Objetivo:** validar conforto e durabilidade físicos antes de ampliar scope.
- **Áreas:** apenas correções específicas demonstradas por testes; docs do
  formato/teste de dispositivo.
- **Dependências:** Fases 1–5 e X4 Pro físico.
- **Aceitação:** calibração/touch, refresh, heap, SDMMC e falha de energia
  exercitados; dados continuam legíveis e nenhuma regressão do perfil C3/S3.

## Referências de código

- Projeto principal: `src/activities/reader/EpubReaderActivity.{h,cpp}`;
  `ReaderUtils.h`; `DictionaryWordSelectActivity.{h,cpp}`;
  `EpubReaderFootnoteSelectActivity.{h,cpp}`; `EpubReaderBookmarksActivity.*`;
  `Activity.{h,cpp}`, `ActivityResult.h`;
  `lib/Epub/Epub/{Page,Section,ParsedText}.h/.cpp`;
  `lib/Epub/Epub/blocks/TextBlock.{h,cpp}`;
  `src/BookmarkEntry.h`, `src/util/{BookmarkFile,BookmarkUtil}.cpp`,
  `src/activities/reader/ProgressFile.h`, `lib/Serialization/PersistableStore.cpp`,
  `src/activities/util/KeyboardEntryActivity.{h,cpp}`,
  `src/activities/home/FileBrowserActivity.cpp`, `docs/file-formats.md`.
- CrossInk: `src/activities/reader/{ClipSelectionActivity,WordRef}.h/.cpp`;
  `src/clippings/{ClipTextBuilder,ClippingsManager}.*`;
  `src/{ClippingStore.h,ClippingStore.cpp}`;
  `src/activities/reader/EpubReaderClippingListActivity.cpp`;
  `src/activities/reader/EpubReaderActivity.cpp`;
  `src/activities/home/SavedItemsHomeActivity.cpp`;
  `src/util/BookMoveUtils.cpp`.
