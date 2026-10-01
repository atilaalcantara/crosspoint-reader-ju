# Plano técnico: Study offline no XTEINK X4 Pro

**Escopo deste documento:** planejamento de Study offline no firmware principal. A fundação de dados e a primeira sessão de UI foram implementadas em fases subsequentes; este documento continua como fonte das decisões de produto e arquitetura.

## Resumo executivo

- CrossPlay oferece um exemplo real e maduro: conversão no computador, conteúdo imutável em SD, estado de scheduling separado, leitura por offsets, scheduler FSRS-5 com learning steps, fluxo de UI e testes host do núcleo. Reutilização direta é seletiva: parsers/núcleos puros e ideias de persistência são candidatos; a Activity inteira e o ecossistema de sync/fontes não são.
- Para V1, adotar **um formato próprio, simples e versionado**, inspirado na separação conteúdo/estado e no acesso aleatório de CrossPlay. Não assumir compatibilidade binária `.xanki`, `.apkg` nem interoperabilidade de scheduling com Anki.
- Entregar quatro ratings e sessão útil com scheduler local determinístico, de intervalos simples, persistido por cartão. Não portar FSRS completo antes de existir requisito explícito de fidelidade/sync.
- Ler uma carta por vez do SD; manter cabeçalhos e índices pequenos ou consultáveis, não o deck textual inteiro. Separar pacote imutável do estado mutável e validar comprimento, versão e integridade antes de expor o deck.
- A sessão é uma Activity própria baseada em `UiListActivity` para renderização e input; a lista também usa `UiListActivity`. Home recebeu a entrada Study sem redesign.

## A. CrossPlay Study: arquitetura e fluxo reais

### Caminho de dados

1. `tools_local/study/study.py` coordena setup/sync/status (`cmd_setup`, `cmd_sync`, `cmd_status`). O conversor de coleção `tools_local/study/anki_to_deck.py` lê SQLite da coleção Anki e escreve os artefatos de deck. `deck_to_anki.py` aplica o histórico de volta à coleção; é uma direção de sincronização adicional, fora da primeira versão deste produto.
2. No cartão, o app procura diretórios de deck sob `/study/`; CrossPlay documenta que um diretório com `meta.dat` representa um deck. `StudyActivity::findDeckDirs`, `openDeck`, `openDeckAt` e `beginDeckSession` descobrem/abrem o deck, criam contexto de scheduler e calculam a fila.
3. `StudyDeck::openMeta` e `openDeck` validam os dados. `StudyDeck::loadNote` usa o índice no arquivo para carregar um registro; `loadCard`/`storeCard` acessam estado pelo mesmo índice. `ByteSource` separa parser de I/O e permite testes host sem HAL.
4. `StudyActivity::takeNext`/`loadCurrent` selecionam e carregam a carta. A view alterna entre `Deck`, `Card`, `Image`, etc.; `Face::Question`/`Answer` controlam frente/verso. Toque revela; os quatro ratings chamam `grade`, que passa pelo `Scheduler::answer`, persiste (`persist`/`flushWrites`) e avança. Há fila limitada para sessão e cartões de learning que voltam em minutos (`kMaxQueue`, `kMaxLearning`).
5. `saveResumeState`/`armResume` persistem carta e face pendentes. `undo` reverte uma resposta recente e invalida o item no log sem truncar arquivo. `StudyStats` deriva estatísticas do estado/log; `meta.dat` contém configuração FSRS e limites.

### Arquivos/símbolos de referência

| Responsabilidade | Evidência no CrossPlay |
|---|---|
| Entrada, sessão, tela, navegação, ratings, persistência | `src/apps_local/study/StudyActivity.{h,cpp}`: `onEnter`, `loop`, `render`, `openDeck`, `beginDeckSession`, `takeNext`, `grade`, `persist`, `flushWrites`, `undo`, `saveResumeState` |
| Leitor binário de conteúdo/estado | `StudyDeck.h` (`ByteSource`, `WritableByteSource`, `StudyDeck`, `Note`, `CardState`) e `StudyDeck.cpp` |
| Scheduler | `StudyScheduler.h/.cpp`: `Scheduler::answer`, `preview`, `isDue`, estados New/Learning/Review/Relearning/Suspended |
| FSRS | `StudyFsrs.h/.cpp`: `Fsrs::review`/previsões; parâmetros transferidos em metadados |
| Texto/layout | `StudyText.h`; renderização/wrap e marcas sob responsabilidade da activity |
| Imagens | `StudyImages.h/.cpp`; imagem lida por bandas, não exige carregar bitmap inteiro |
| Estatísticas | `StudyStats.h/.cpp`; contagem inclui leitura do histórico e exclui reviews anulados |
| Conversão | `tools_local/study/anki_to_deck.py`; configuração/coordenação em `study.py`; retorno ao Anki em `deck_to_anki.py` |
| Contrato binário | `docs/apps/study-deck-format.md` |
| Compatibilidade | `docs/apps/study-anki-compatibility.md`, `docs/apps/study.md` |
| Testes host | `host-tests/study/` testa parser, FSRS, scheduler, texto e estatísticas; a Activity completa depende de Arduino/HAL/painel |

### Semântica, formatos e limites

- `deck.dat` guarda conteúdo somente-leitura; `cards.dat` guarda um estado fixo de 32 bytes por nota; `revlog.dat` é append-only de 32 bytes por revisão; `meta.dat` guarda pesos/configuração; existe também `.resume`. Um deck demonstrativo documentado contém 5.001 cartões e ~900 KB de texto.
- O formato `deck.dat` atual usa magic `XSTUDYD\0`, versão 3, número de campos, quantidade, LUT de offsets de 32 bits com sentinela e registros compostos por comprimentos `uint16` little-endian + UTF-8. V2 (7 campos) segue legível após o campo de cloze ser adicionado em V3 (8 campos). Campo por campo é específico ao perfil HSK/Mandarin; não é modelo universal de note type Anki.
- `cards.dat` está posicionalmente alinhado com notas. Estado tem ID Anki, estabilidade/dificuldade, due day/minute, última revisão, repetições, lapses, estado e step. `revlog.dat` mantém ID, timestamp, rating, estado, intervalos, duração (zero deliberadamente) e flag de undo.
- `meta.dat` carrega 19 pesos FSRS-5, retenção, intervalos/limites, origem temporal e até 6 learning/relearning steps. O scheduler combina FSRS com steps, mas não replica todo Anki: sem fuzz, leeches/burying completo; há diferenças FSRS documentadas e casos não suportados.
- Decks/subdecks: o leitor CrossPlay descrito no formato os descobre como diretórios independentes, ordena pelo nome e permite alternar decks; não implementa uma árvore de subdecks equivalente à hierarquia do Anki. Nome e slug são escolhidos pela ferramenta local.
- Conteúdo HTML não é renderizado como template/CSS Anki no dispositivo. Conversão limpa/achata campos. Cloze é pré-renderizado nas faces no conversor. Imagens são pré-empacotadas em `images.dat`, áudio é descartado; LaTeX/templates arbitrários não são interpretados. Ruby/furigana e fontes CJK são extensões específicas do deck de Mandarin, não requisitos gerais.
- Arquivos inválidos/truncados devem falhar na abertura, não desaparecer silenciosamente. O formato documenta limites por registro (nota até 1.024 bytes em UTF-8 preparado) e validação no conversor, para que o firmware não descarte conteúdo de forma invisível.

## B. Formato recomendado

### Decisão

Criar um **formato Study V1 do CrossPoint**, documentado e versionado. Não consumir `.apkg` no firmware: é um pacote de coleção Anki, complexo e fora do papel do ESP32. Não declarar compatibilidade com o formato binário CrossPlay, pois seus campos, FSRS, IDs, histórico e sincronização são específicos. Reutilizar os mecanismos que provaram valor: conteúdo imutável, estado mutável em arquivo distinto, offsets para acesso aleatório, UTF-8, conversor responsável por normalização e leitura via interface testável.

### Contrato implementado em `docs/study-v1-format.md`

Pacotes `.cps` ficam diretamente em `/study/`; cada pacote contém decks e cards em uma hierarquia. Os arquivos mutáveis ficam ao lado do pacote:

```text
/study/<package>.cps             # conteúdo versionado, imutável
/study/<package>.state           # estado fixo por card, mutável e ligado ao CRC do pacote
/study/<package>.session         # checkpoint pequeno, substituição recuperável
```

- Header de 40 bytes com magic `CPSTUDY\0`, versão 1, endian little-endian, contagens/offsets/tamanho e CRC-32 do payload; tabelas de deck fixas (108 bytes) e índice de offsets de card (4 bytes/card em disco). O contrato formal, limites e validações ficam em `docs/study-v1-format.md`.
- IDs de deck são `uint32` não zero; IDs de card são `uint64` não zero e estritamente ascendentes/únicos no arquivo. Faces são UTF-8 plain text até 4 KiB cada; LF é line break, sem HTML/template executável.
- Hierarquia é representada por `parentDeckId`; a UI mostra uma lista achatada com marcadores de nível. Escolher qualquer deck inclui seus descendentes; texto de cards não é carregado para montar a lista.
- `.state` usa duas slots alternadas de 40 bytes por card (80 bytes/card) com geração e CRC por slot. O header vincula CRC/count do `.cps`; updates escrevem apenas a slot inativa. `.session` é checkpoint de 48 bytes vinculado ao pacote/deck/card/face.
- Limites V1: 1.024 decks/pacote, 100.000 cards/pacote, nome 96 bytes, faces de 4 KiB e até 64 parágrafos desenhados por face na primeira UI. O parser mantém 8 bytes/card em RAM para offset e membership; contagens maiores exigem validação de heap/PSRAM no hardware.

Os limites são proposta de V1, não propriedades medidas de um aparelho. Confirmar bytes/offsets com teste de deck gerado e usar tipos/guards que não permitam wraparound. Se um limite operacional diferente for escolhido, alterar documento e geradores conjuntamente.

### Por que não copiar `.xanki` integral

O layout CrossPlay resolve muito bem um cliente FSRS sincronizável e campos estruturados específicos: revisão de estado por seek, revlog reimportável e parâmetros por preset. Isso traz complexidade (histórico, relógio Anki, IDs externos, steps FSRS, regras de sync) que o fluxo offline V1 não precisa. A estrutura de 8 campos também fixa decisões de layout/template do CrossPlay. Adotar o mesmo princípio e reespecificar o contrato evita compatibilidade acidental falsa.

## C. Gap analysis: portar / adaptar / reimplementar / descartar

| Classe | Itens |
|---|---|
| Reutilizar como referência/conceito | `ByteSource`/`WritableByteSource` desacoplando I/O e lógica; offsets para leitura direta; content/state separados; limites explícitos e testes host; converter HTML/cloze antes do dispositivo; validação de arquivo antes da sessão. Confirmar licenças e dependências antes de copiar código. |
| Adaptar | Parser freestanding de deck a formato próprio; estratégia de estado de cartão fixo; fila pequena; máquina de frente/verso/rating; retorno ao deck browser; persistência de checkpoint. HAL do alvo precisa usar `Storage`/`HalFile`, não HAL ou classes do CrossPlay. |
| Reimplementar | Descoberta de deck com hierarquia; tela/lista com `UiListActivity`; tela de card via `UiAppHost` ou atividade própria; scheduler local mínimo; estado e gravação com contratos do `HalStorage`; integração Home/ActivityManager; catálogo/testes de simulator. |
| Fora da V1 | Sync Wi-Fi/AnkiWeb, pareamento, `deck_to_anki.py`, SQLite no firmware, FSRS/suspensões/estatísticas avançadas, imagens/áudio, fontes CJK, Ruby, templates CSS, converter/instalador do CrossPlay, dependências de rede/site e jogos/apps restantes. |

## D. Arquitetura proposta no CrossPoint

1. **Home/rota:** `HomeActivity` tem item `Study`, integrado via `ActivityManager::goToStudy`. Home continua sem redesign.
2. **Catálogo:** `StudyDeckListActivity` usa `UiListActivity`; `Study::discoverFromSd` varre até 512 arquivos `.cps`, valida cada um e descarta os índices de card depois de copiar metadados necessários. Linhas são achatadas, ordenadas por profundidade e exibem contagem de subtree; inválidos não escondem os válidos.
3. **Sessão:** `StudySessionActivity` herda `UiListActivity` para fluxo FreeInkUI/chrome/ações; `StudyPackage` mantém lookup por offset e carrega somente o card atual; `StudyScheduler` é lógica pura; `PersistentState` usa `Storage`/`HalFile` e arquivos separados do pacote.
4. **Responsabilidades:** parser valida bytes/versões/CRC; catálogo resolve pai/descendente; store é único dono do I/O; activity orquestra sessão e desenho; renderer nunca faz varredura do SD. Abrir recursos em `onEnter`, fechar em `onExit`, sem trabalho assíncrono que mantenha referência após destruir Activity.
5. **UI strings:** todas via `tr(STR_*)` nas traduções EN e PT-BR. Botões/ações usam input lógico `MappedInputManager`, não pinos. Touch pelo fluxo comum FreeInkUI. Orientar com dimensões/margens atuais; X4 Pro físico tem touch GT911, dois botões laterais e Home capacitivo (não presumir quatro botões frontais).

Pontos de integração confirmados: `src/activities/ActivityManager.h` oferece `replaceActivity`, `pushActivity`, `popActivity`; `Activity.h` possui ciclo de vida; `UiListActivity.h` define contrato de linha e navegação touch/botões; `UiAppHost` é indicado no `AGENTS.md` para layouts próprios. A Home é uma superfície legada e não deve ser redesenhada.

## E. Modelo de dados

- **DeckInfo:** `id`, `parentId` opcional, `name`, contagem, identidade/hash do arquivo, offsets/range dos seus cartões. Diretório/pasta é localização, não identidade semântica.
- **Card:** `id` estável, `front` e `back` UTF-8. Face pode ter múltiplas linhas; não há markup executável. Conteúdo imutável após importação.
- **CardState:** status New/Learning/Review/Suspended, due day, interval, último dia/rating, reps/lapses. Again fica Learning até o retry pass; não há due minute.
- **ReviewSession (RAM):** deck, fase Main/Again/Done, cursor, card/face atuais; não retém fila nem conteúdo integral. Vencidos/New percorrem índices crescentes, e a passagem Again revisita os estados Learning de hoje.
- **Persistência:** um update alterna slot fixa do card e flush; checkpoint `.session` grava índice/ID/face via `.tmp/.bak`. Reboot restaura só se pacote, deck, índice, ID e elegibilidade ainda corresponderem.

## F. Scheduler

### Alternativas

- **A — sessão sem scheduler persistente:** simplest; sempre lista/embaralha, ratings só movem para próxima carta. Fácil, mas “Again/Hard/Good/Easy” perdem significado entre sessões e não há progresso útil.
- **B — scheduler simples local (implementado):** estado enxuto por cartão, passagem principal de New/vencidos em ordem de índice e uma passagem final de retry para Again. Intervals determinísticos descritos abaixo; sem algoritmo Anki/FSRS ou promessa de equivalência.
- **C — importar/exportar scheduling:** exige IDs Anki estáveis, regra para histórico/conflitos e conversor que leia/atualize a coleção. Fora do fluxo sem sync de V1.
- **D — compatibilidade maior com Anki:** FSRS + steps, parâmetros, fuzzeamento, limites diários, histórico/revlog, regras de irmãos/suspensão e compatibilidade temporal. Alto custo/teste e assume requisito ainda não pedido.

Decisão V1: ratings persistidos e agenda local previsível, sem scheduler “Anki-compliant”. Usa dias UTC de `time(nullptr)/86400`; se o relógio não estiver disponível, dia zero mantém cartões novos estudáveis e reviews com vencimento futuro aguardando. Again entra uma vez numa passagem de retry no fim da passagem principal; Again no retry agenda para amanhã. Hard inicial = 1 dia; Good = 3; Easy = 5. Para cards já revisados, Hard = max(intervalo+1, arredondamento de 1,2×), Good = max(intervalo+1, 2×) e Easy = max(intervalo+2, 3×). Não existe FSRS, fuzz, learning em minutos ou histórico de reviews.

## G. UX V1

1. Home → `Study` → lista hierárquica achatada, com prefixos de profundidade e contagem de subtree; selecionar qualquer deck inclui descendentes. V1 não tem affordance separada de expand/collapse.
2. Confirm/tap num deck abre resumo curto (nome, vencidos/novos, iniciar); Back retorna à lista com expansão/seleção preservadas.
3. Sessão: front ocupa área principal, footer com affordance grande **Show answer**. Tap no cartão ou Confirm revela verso; não marcar resposta ao revelar.
4. Verso: mostrar resposta e quatro alvos claros Again / Hard / Good / Easy com intervalo previsto (se clock confiável). Rating confirma progresso e atualiza cartão uma vez; o próximo cartão vai à frente sem animação.
5. Fim: estado de sessão com estudados/remaining e ações “Done/Back” e “Study again”, sem tela extra se resumo de deck resolve retorno. Não perder resposta já confirmada ao sair/reboot.
6. X4 Pro: touch para alvos amplos; botões Up/Down selecionam opções quando aplicável, Confirm revela/seleciona, Back volta; Home segue navegação global. Não obrigar interação tátil fina.

Contraste alto, sem animação, quantidade pequena de elementos, refresh por transição (front→back→next) e não por polling. Evitar colocar conteúdo do card em widgets que redesenham individualmente. Localizar todas as labels em inglês e português brasileiro. Detalhes de waveform/latência/ghosting só serão confirmados no dispositivo.

## H. Performance, SD e recuperação

- X4 Pro alvo é ESP32-S3 com PSRAM, flash/SD e e-ink 800×480, mas não pressupor alocação ilimitada nem usar PSRAM como desculpa para ler tudo. Decks potencialmente grandes devem usar leituras posicionais e um buffer reutilizável para apenas front/back do cartão.
- A fundação atual usa 4 bytes/card de offsets no arquivo e mantém offset + deck ID (8 bytes/card) em RAM: 80 KB para 10 mil e 800 KB para 100 mil, além de descritores e alocador. O teste host mediu abertura de uma fixture sintética de 10 mil cards em ~0,01 s; o open valida CRC e percorre registros, enquanto a tela só faz I/O para os cards candidatos/atuais. Medir heap real antes de recomendar 100 mil no X4 Pro.
- Validar tamanhos com aritmética sem overflow, limites de campos, EOF, UTF-8 e CRC; recusar deck incompatível/incompleto com mensagem traduzida. Um `deck.bin.tmp` de conversão só pertence ao conversor futuro; firmware abre pacote final somente após consistência completa.
- Cada rating alterna uma slot fixa de 40 bytes dentro do registro de estado de 80 bytes, evitando reescrever o estado do deck; store usa `Storage`/`HalFile` e flush. Checkpoint usa `.tmp`, `.bak` e `.bad` validado. FAT rename/remove não é atomicidade absoluta; slots/CRC e candidatos validados preservam a cópia recuperável após falha.
- Não atualizar todo `state.bin` por rating. Criar estado inicial de forma recuperável durante importação; a cópia do pacote e a primeira criação de estado precisam tolerar cartão cheio/remoção/queda de energia. SD cheio ou erro de gravação bloqueia avanço silencioso: exibir erro e manter resposta/card em tela até retry ou saída controlada.
- A identidade do pacote deve incluir ID e hash/checksum de conteúdo; substituir deck sob mesmo caminho detecta incompatibilidade e permite reset/backup de estado sem misturar scheduling entre decks diferentes.

## I. Contrato e estado do conversor local

O CLI inicial está implementado em `tools/study-converter/convert.py`, com o guia de uso em `docs/STUDY_CONVERTER.md`. Ele recebe `.apkg`, permite `--deck` para selecionar deck/subtree e cria `.cps` pronto para copiar por USB/SD. O suporte concreto é limitado às variantes SQLite descritas no guia. A futura GUI/integração de conveniência continua separada. O conversor produz a especificação acima e:

- converter nome/árvore/IDs estáveis e cards estudáveis; preservar Unicode, quebras de linha e campos textuais escolhidos;
- renderizar o subconjunto implementado (`{{Field}}`, `{{text:Field}}`, `{{FrontSide}}`, condicionais simples); HTML vira texto. Firmware não executa HTML, JavaScript, CSS, áudio ou template;
- materializar cloze básico não aninhado como frente/verso por card; cloze aninhado/sintaxe não suportada é avisada e os cards afetados não são escritos;
- gerar índices/offsets, tamanho total, checksum, version e card IDs sem duplicatas; garantir que limites por campo e total foram satisfeitos e apresentar ao usuário aviso de conteúdo não suportado/truncado;
- produzir deck sem SQLite no firmware/AnkiWeb/serviço remoto; converter no computador. O firmware abre `.cps` e associa `<basename>.state`/`.session`; V1 não escreve scheduling de volta no `.apkg`.

CrossPlay demonstra pipeline de coleção local → conversão (`anki_to_deck.py`) e separação de sync; sua saída não deve ser consumida como contrato deste firmware. O conversor deste projeto declara um subconjunto, não fidelidade universal para templates/decks Anki arbitrários.

## J. Simulator e testes

- Testes puros host do parser/offset/CRC/UTF-8/versão, scheduler, transições, resolução de subdeck, checkpoint e recuperação de writes simulados; incluir fixtures válidas pequenas, UTF-8 multilinha, truncadas, campos excessivos, offset fora do arquivo, versão desconhecida, estado parcialmente gravado e 10k+ cartões gerados.
- Simulador integrado deve testar pela UI: entrada Home, expand/collapse, iniciar deck/subdeck, frente, reveal, ratings, próximo, Back, sair/reabrir e persistência entre processos usando `fs_/`. Validar erros de arquivo, Unicode, deck grande e estado já existente; scripts de input do simulador (`CROSSPOINT_SIM_INPUT_SCRIPT`) automatizam taps/botões.
- Build `pio run -e simulator_x4`, `pio run -e x4pro`; testes `cmake --build build/host-tests` e `ctest --test-dir build/host-tests --output-on-failure`. O simulador exercita fluxo lógico/renderização SDL e não valida GT911 físico, SDMMC, temporização, ghosting ou waveform e-ink (limites em `docs/DEVELOPMENT_SETUP.md`).
- Teste físico posterior no X4 Pro para alvo touch, legibilidade/tamanho, botões, refresh, ghosting, bateria e operação com cartão real. **Hardware validation pending — X4 Pro currently unavailable.** Isso não bloqueia implementação/teste host/simulator.

## K. Riscos e decisões abertas

Resolvidos pela inspeção: CrossPlay não usa deck Anki diretamente no firmware; formato atual é binário próprio; sua descoberta de deck não representa árvore de subdecks; UI e HAL não são portáveis literalmente; o projeto principal fornece Home/Activity, `UiListActivity`, `UiAppHost`, `Storage` e simulator.

Decisões futuras ainda abertas:

1. Extensão futura do conversor: note types/template filters avançados, Cloze aninhado/overlap e mídias continuam fora do subconjunto declarado; novos suportes devem ser adicionados com fixtures antes de ampliar a promessa.
2. Ergonomia, legibilidade, waveform/ghosting e latência precisam de aparelho físico. Hardware validation pending — X4 Pro currently unavailable.
3. O conversor passou por validação host/simulator com uma exportação real APKG v3; profundidade/quantidade continuam limitadas pelo contrato do pacote. Compatibilidade de glyphs, touch e desempenho real seguem dependentes do X4 Pro físico.

## L. Plano incremental futuro

### Fase 1 — especificação congelada + fixtures (concluída)
- **Objetivo:** aprovar nome, regra de árvore, scheduler inicial, limites e contrato binário; publicar golden fixtures do pacote.
- **Entrega:** formato V1 e amostras/fixture gerada manualmente ou por ferramenta de teste; sem UI funcional.
- **Áreas:** `docs/ANKI_STUDY_PLAN.md`, `docs/file-formats.md` ou novo `docs/study-format.md`, host tests.
- **Testes:** parser fixtures, versões/corrupção/limites, casos Unicode.
- **Aceitação:** bytes e campos documentados sem ambiguidade; fixture aceita/rejeita segundo expected.

### Fase 2 — reader/store/scheduler puros (concluída)
- **Objetivo:** ler deck por offsets, resolver hierarquia e gravar estado local.
- **Entrega:** bibliotecas puras e storage adapter HalFile; scheduler deterministicamente testável.
- **Áreas:** novo `src/activities/study/` ou `src/study/`, `lib/hal/HalStorage`, `test/`.
- **Testes:** host tests para indexação, card lookup, subtrees, answers, reboot/checkpoint e falhas de write.
- **Aceitação:** deck grande não carrega conteúdo integral; resposta altera somente estado necessário e recuperação não perde/corrompe deck.

### Fase 3 — browser e sessão Study (concluída em host/simulator)
- **Objetivo:** disponibilizar Study navegável com decks de fixtures instalados manualmente.
- **Entrega:** Home entry, deck browser expansível, front/back, reveal, 4 ratings, próximo/fim, recuperação de sessão.
- **Áreas:** `HomeActivity`, `ActivityManager`, novas Activities FreeInkUI, traduções EN/PT-BR.
- **Testes:** simulator X4: navegação touch/botões e fluxo de sessão em ambos os idiomas; testar Back e reabertura.
- **Aceitação:** nenhum cartão confirmado desaparece após reinício; UI legível e usável sem touch.

### Fase 4 — robustez, limites e integração
- **Objetivo:** fechar comportamento de erro/performance sem conversor final.
- **Entrega:** mensagens de arquivo inválido/SD cheio, limites visíveis, medição com decks 10k/100k, documentação de instalação manual temporária.
- **Áreas:** parser/store, host tests, simulator e docs.
- **Testes:** arquivo truncado, versão inválida, UTF-8, stress, falha/interrupção em cada etapa de persistência, build simulator e `x4pro`.
- **Aceitação:** falhas são reportadas sem corromper conteúdo; RAM/latência são medidas e adequadas no X4 Pro perfil; hardware pendente se indisponível.

### Fase 5 — conversor desktop local (CLI e validação APKG real concluídos)
- **Objetivo:** `.apkg` → pacote V1 pronto para SD, sem rede.
- **Entrega:** CLI Python local para escolher deck/subtree, conversão com relatório, suportes/recusas explícitos e escrita atômica. GUI continua separada.
- **Áreas:** ferramenta desktop nova e fixtures compartilhadas; não converter lógica Anki para firmware.
- **Testes:** APKGs sintéticos cobrindo templates/Cloze/media/Unicode/subtree e pacote validado pelo parser C++; conversão de APKG real v3, filtro de subdeck, determinismo, parser C++ e sessão simulator concluídos. Hardware físico permanece pendente.
- **Aceitação:** copiar `.cps` para o cartão e usar firmware sem conta/serviço externo; relatório deixa explícito o conteúdo omitido.

### Fase 6 — teste físico e eventual evolução
- **Objetivo:** avaliar ergonomia/refresh no X4 Pro e decidir se scheduler/compatibilidade merece expansão.
- **Entrega:** checklist e correções reproduzíveis; decisão separada para FSRS/import-export de scheduling.
- **Áreas:** UI/input/render/SD e dispositivo.
- **Testes:** touch, dois botões, Unicode real, sessões longas, restart/SD real, ghosting/refresh.
- **Aceitação:** nenhuma regressão física conhecida e limites/pendências registrados; FSRS/sync somente com requisito independente.

## Fontes principais consultadas

- CrossPlay: `docs/apps/study-deck-format.md`, `docs/apps/study.md`, `docs/apps/study-anki-compatibility.md`, `src/apps_local/study/StudyActivity.{h,cpp}`, `StudyDeck.{h,cpp}`, `StudyScheduler.{h,cpp}`, `StudyFsrs.{h,cpp}`, `StudyText.h`, `tools_local/study/anki_to_deck.py`, `study.py`, `deck_to_anki.py`, `host-tests/study/`.
- Firmware principal: `AGENTS.md`, `docs/DEVELOPMENT_SETUP.md`, `docs/HIGHLIGHTS_NOTES_STATUS.md`, `docs/file-formats.md`, `src/activities/ActivityManager.h`, `Activity.h`, `UiListActivity.h`, `components/UiAppHost.*`, `home/HomeActivity.*`, `lib/hal/HalStorage.*`, `src/util/HighlightStore.*`.
