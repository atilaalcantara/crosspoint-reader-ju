# Manual prático: testar localmente sem o X4 Pro

Este manual é para executar no Mac e explorar manualmente o que já existe no Reader, Highlights/Notes e Study. O simulador compila e abre neste checkout, mas a interface deve ser operada por você (mouse/toque simulado e teclado). Build/execução host não equivalem à validação do aparelho físico.

## Comece por aqui: usar como usuário, sem configurar variáveis

O caminho mais simples é este:

1. **Anki:** escolha seu arquivo `.apkg` e converta-o no Mac para `.cps`. O simulador não abre `.apkg`: Study lê o `.cps` convertido.
2. **Copie para o SD simulado:** o helper cria `/study/` e copia o `.cps` para lá. Opcionalmente, copie também um EPUB para `/books/`.
3. **Abra o simulador:** um comando compila, escolhe automaticamente o SD de teste certo e abre a janela. Não precisa digitar `export` nem configurar nada no Terminal.

### A. Converter seu próprio deck Anki com Python

Exemplo para um APKG na pasta Downloads. Troque `MeuBaralho.apkg` pelo nome real do seu arquivo:

```bash
cd "/Users/atilaalcantara/Documents/ankiehighlightscrosspoint/crosspoint-reader-ju"
python3 tools/study-converter/convert.py "$HOME/Downloads/MeuBaralho.apkg" "$HOME/Downloads/MeuBaralho.cps"
```

O resultado é `MeuBaralho.cps` em Downloads. A saída informa quantos cards converteu, ignorou e quaisquer warnings. Se quiser escolher só um subdeck, acrescente `--deck "Nome completo do deck::Subdeck"`; o nome precisa corresponder ao pacote. O comando para listar nomes é:

```bash
tools/study-converter-dotnet/artifacts/validation-cli/osx-arm64/StudyConverter.Cli "$HOME/Downloads/MeuBaralho.apkg" --inspect
```

### B. Converter com o aplicativo .NET

Abra `tools/study-converter-dotnet/artifacts/StudyConverter.app` pelo Finder. Arraste seu `.apkg` (ou **Escolher arquivo**), selecione **Todos os decks** ou o subdeck na lista, clique **Converter**, escolha onde salvar o `.cps` e leia o resumo/warnings. **Mostrar arquivo** abre a pasta onde foi salvo.

### C. Copiar o CPS (e opcionalmente EPUB) para o SD de teste

Troque os caminhos de exemplo pelos caminhos dos seus arquivos. Se não for testar Reader agora, omita a opção `--epub`:

```bash
cd "/Users/atilaalcantara/Documents/ankiehighlightscrosspoint/crosspoint-reader-ju"
python3 tools/simulator-test/prepare_sd.py --reset \
  --cps "$HOME/Downloads/MeuBaralho.cps" --cps-name MeuBaralho.cps \
  --epub "$HOME/Documents/MeusLivros/MeuLivro.epub"
```

`--reset` limpa **somente** `crosspoint-reader-ju/build/simulator-test-sd`, o SD fictício controlado para estes testes, e depois copia os arquivos. Isso apaga também resultados anteriores do simulador (progresso Study, highlights e notas) dentro dessa sandbox. Sem `--reset`, os arquivos/progresso anteriores são preservados. No Mac, o resultado fica em:

```text
crosspoint-reader-ju/build/simulator-test-sd/study/MeuBaralho.cps  ← arquivo `/study/MeuBaralho.cps` no firmware
crosspoint-reader-ju/build/simulator-test-sd/books/MeuLivro.epub  ← arquivo `/books/MeuLivro.epub` no firmware
```

### D. Abrir e usar o firmware simulado

```bash
bash tools/simulator-test/run.sh
```

Esse comando **compila e abre** o simulador já apontado para a sandbox acima. Feche a janela SDL para sair; os arquivos ficam no SD de teste até você usar `--reset`.

- **Estudar Anki:** Home → **Study** → `MeuBaralho` → escolha o deck/subdeck → abra card → **Mostrar resposta** → escolha **Again**, **Hard**, **Good** ou **Easy**.
- **Ler livro:** Home → **Browse Files** → `books` → abra `MeuLivro.epub`.
- **Criar highlight:** no texto do EPUB, mantenha o mouse pressionado sobre uma palavra, arraste até o final do trecho e solte; escolha **Done**. Mouse/arraste simulam toque.
- **Adicionar nota:** abra menu do Reader → **Highlights** → mantenha pressionada uma linha → **Add note** → digite no teclado na tela e confirme. A linha passa a indicar que tem nota. Reabra a lista para editar; Back/cancelar descarta a edição. Reinicie sem `--reset` para verificar que a nota e o highlight continuam.
- **Ver arquivos:** Finder → `crosspoint-reader-ju/build/simulator-test-sd`. O CPS e os estados ficam em `study/`; EPUB em `books/`; dados de highlights/notas em `.crosspoint/highlights/`.

Os detalhes, comandos de comparação, formatos e testes controlados estão nas seções seguintes. Se você quiser apenas começar hoje, siga A (ou B), C e D.

> Os comandos técnicos abaixo usam caminhos descobertos neste checkout. O APKG do ENARE é apenas um exemplo local em Downloads e não é versionado.

## 1. Pré-requisitos e diretórios

Os roteiros em **Comece por aqui** não exigem variáveis. Para copiar os comandos técnicos seguintes, defina estas duas abreviações no Terminal:

```bash
export WORKSPACE="/Users/atilaalcantara/Documents/ankiehighlightscrosspoint"
export PROJECT="$WORKSPACE/crosspoint-reader-ju"
cd "$PROJECT"
```

São necessários PlatformIO (`pio`), SDL2 (`sdl2-config`), Python 3, CMake, compilador C++ e, para recompilar/CLI gerenciado, .NET SDK 10. O simulador já foi compilado no ambiente atual. O CLI de publicação .NET é self-contained, portanto não depende do SDK para executar.

O APKG ENARE encontrado (nome no filesystem em Unicode NFD) é:

```bash
export APKG="/Users/atilaalcantara/Downloads/Preparatório R1 Vet - ENARE.apkg"
test -f "$APKG" && printf 'APKG localizado: %s\n' "$APKG"
```

Não reescreva os acentos manualmente se o shell não encontrar o arquivo; obtenha o caminho real com `python3 -c 'import os; print([repr(x) for x in os.listdir("/Users/atilaalcantara/Downloads") if "ENARE.apkg" in x])'` e use o nome listado.

EPUB fixture existente: `$PROJECT/test/epubs/test_reader_menu_progress.epub` (8.553 bytes; contém capítulos XHTML curtos). O helper preserva o basename e instala como `/books/test_reader_menu_progress.epub`.

## 2. Build e launch do simulador X4 Pro

O perfil `simulator_x4` está em `platformio.sim.ini` e define `SIMULATOR_DEVICE_X4_PRO`. Build, a partir de `crosspoint-reader-ju/`:

```bash
pio run -c platformio.sim.ini -e simulator_x4
```

O executável produzido é:

```text
$PROJECT/.pio/build/simulator_x4/program
```

Para **compilar e abrir já com o SD correto**, use este comando único. Ele prepara a sandbox se ainda não existir, compila e abre o simulador:

```bash
bash tools/simulator-test/run.sh
```

Para apenas compilar, sem abrir a janela:

```bash
pio run -c platformio.sim.ini -e simulator_x4
```

Feche a janela SDL para encerrar. Teclas mapeadas pela dependência atual: setas para navegação, Return selecionar, Escape voltar, `H` Home capacitivo, mouse para tap/arrastar. No reader, use clique pressionado/arraste para simular touch e gesto. A dependência também oferece script sintético de input, mas os roteiros abaixo são para interação manual.

## 3. O SD virtual: localização e equivalência

O mock do cartão lê `CROSSPOINT_SIM_SD`. O script `bash tools/simulator-test/run.sh` define essa opção automaticamente para `build/simulator-test-sd`; você não precisa defini-la. Sem essa opção, o simulador cai em `./fs_`, que depende do diretório de trabalho; evite esse modo para os testes deste guia.

Este manual usa:

```text
/Users/atilaalcantara/Documents/ankiehighlightscrosspoint/crosspoint-reader-ju/build/simulator-test-sd
```

Essa pasta no Finder representa a raiz `/` que o firmware vê. Exemplo:

```text
Mac: .../build/simulator-test-sd/study/enare.cps
Firmware: /study/enare.cps
```

Abra a raiz no Finder:

```bash
open "$PROJECT/build/simulator-test-sd"
```

Inspecione no Terminal:

```bash
ls -la "$PROJECT/build/simulator-test-sd"
find "$PROJECT/build/simulator-test-sd" -type f -print
```

O diretório antigo `$PROJECT/fs_` é a raiz fallback por cwd e já contém `/.crosspoint/` neste ambiente. Preserve-o; não o use como sandbox de testes nem o apague. A variável `CROSSPOINT_SIM_SD` substitui a raiz toda, não monta uma cópia/overlay.

### Preparar, resetar e snapshot com proteção

O helper novo `tools/simulator-test/prepare_sd.py` controla somente `build/simulator-test-sd`, exige marcador próprio antes de resetar e recusa paths/symlinks diferentes. Não apaga nada fora dessa sandbox.

Inicializar e incluir fixture e CPS:

```bash
cd "$PROJECT"
python3 tools/simulator-test/prepare_sd.py \
  --epub "$PROJECT/test/epubs/test_reader_menu_progress.epub" \
  --cps /tmp/enare-python.cps --cps-name enare.cps
```

Resetar **somente** a sandbox marcada e deixá-la vazia:

```bash
python3 tools/simulator-test/prepare_sd.py --reset
```

Resetar e preparar de novo:

```bash
python3 tools/simulator-test/prepare_sd.py --reset \
  --epub "$PROJECT/test/epubs/test_reader_menu_progress.epub" \
  --cps /tmp/enare-python.cps --cps-name enare.cps
```

Antes de um cenário de recuperação, faça snapshot para um destino ainda inexistente:

```bash
python3 tools/simulator-test/prepare_sd.py --snapshot \
  "$PROJECT/build/snapshots/study-before-recovery"
```

Para conservar dados entre execuções, não rode `--reset` e mantenha o mesmo `CROSSPOINT_SIM_SD`. Fechar a janela do simulador não limpa arquivos. O helper não apaga a pasta snapshot.

## 4. Conversor Python

O conversor atual é `tools/study-converter/convert.py`; não exige dependências Python extras. Confirme o input e converta o deck completo:

```bash
python3 tools/study-converter/convert.py "$APKG" /tmp/enare-python.cps
```

No pacote atual, o nome exato do subdeck é `Preparatório R1 Vet - ENARE::2. Virologia` (o nome raiz do deck é parte do nome completo). Converter apenas esse subdeck:

```bash
python3 tools/study-converter/convert.py "$APKG" /tmp/enare-virologia-python.cps \
  --deck "Preparatório R1 Vet - ENARE::2. Virologia"
```

Sem `--deck`, são incluídos os decks do package. Com `--deck`, converte o deck indicado e seus descendentes. O relatório final deve mostrar `Cards origem` implícitos no APKG conhecido: 1.039, `Cards convertidos: 1036`, `Cards ignorados: 3`, e três avisos Cloze sobre ordinais sem exclusão correspondente. São skips deliberados, não falha silenciosa. O teste realizado também reportou 11 decks, 219 Cloze incluídos/3 ignorados, 14 mídias detectadas (não copiadas), 15 referências de imagens omitidas e nenhum áudio/LaTeX omitido.

Para checar o subdeck via conversão e contagem:

```bash
python3 tools/study-converter/convert.py "$APKG" /tmp/enare-virologia-python.cps \
  --deck "Preparatório R1 Vet - ENARE::2. Virologia"
build/host-tests/study_v1/study_package_check /tmp/enare-virologia-python.cps --open-only
```

Esperado: `30` cards convertidos, zero skips para esse subtree e checker `OK`. Se o checker não existir, faça a seção de build de testes host abaixo.

## 5. Conversor .NET — GUI macOS

Bundle publicado existente:

```text
$PROJECT/tools/study-converter-dotnet/artifacts/StudyConverter.app
```

No Finder, use **Ir → Ir para a Pasta…** (`Cmd+Shift+G`) e cole o caminho acima. Abra `StudyConverter.app`. Se o macOS pedir confirmação por ser um build local, use o fluxo normal de segurança do macOS para app baixado/local.

Também pode abrir pelo Terminal:

```bash
open "$PROJECT/tools/study-converter-dotnet/artifacts/StudyConverter.app"
```

Passos da interface:

1. Arraste o arquivo `$APKG` para a janela ou clique **Escolher arquivo** e selecione o `.apkg`.
2. Aguarde a inspeção. A janela mostra quantidade de cards/decks e warnings; para este arquivo deve indicar 1.039 cards e 11 decks.
3. Em **Deck a converter**, deixe **Todos os decks** para o package inteiro; selecione `Preparatório R1 Vet - ENARE::2. Virologia · 30 cards` para Virologia.
4. Clique **Converter**. No diálogo salvar, escolha `/tmp/enare-dotnet-gui.cps` ou outra pasta sua e confirme.
5. Leia o status: número convertido, ignorados e warnings aparecem na mesma janela. Para ENARE completo espere 1.036 convertidos e 3 ignorados.
6. Clique **Mostrar arquivo** para abrir a pasta de saída no Finder. O app não usa automação de acessibilidade neste roteiro.

## 6. Conversor .NET — CLI self-contained

CLI publicado para este Mac Apple Silicon:

```text
$PROJECT/tools/study-converter-dotnet/artifacts/validation-cli/osx-arm64/StudyConverter.Cli
```

Package inteiro:

```bash
"$PROJECT/tools/study-converter-dotnet/artifacts/validation-cli/osx-arm64/StudyConverter.Cli" \
  "$APKG" /tmp/enare-dotnet.cps
```

Subdeck (o terceiro argumento posicional é o nome de deck completo):

```bash
"$PROJECT/tools/study-converter-dotnet/artifacts/validation-cli/osx-arm64/StudyConverter.Cli" \
  "$APKG" /tmp/enare-virologia-dotnet.cps \
  "Preparatório R1 Vet - ENARE::2. Virologia"
```

Para apenas inspecionar nomes/contagens antes de converter:

```bash
"$PROJECT/tools/study-converter-dotnet/artifacts/validation-cli/osx-arm64/StudyConverter.Cli" \
  "$APKG" --inspect
```

O resumo esperado inclui `Decks: 11; converted: 1036; skipped: 3` para o package inteiro. A publicação usa runtime self-contained e é específica para `osx-arm64`.

## 7. Provar que Python e .NET geraram exatamente o mesmo CPS

O script diferencial compara estrutura, IDs, conteúdo, decks, skips e bytes. Rode-o com o APKG ENARE:

```bash
cd "$PROJECT"
python3 test/study_converter/differential_dotnet.py "$APKG"
```

Ele cria CPS temporários, invoca Python e o DLL de release do CLI .NET, calcula SHA-256, roda `StudyPackageCheck` nos dois quando disponível e termina com `PARITY` se bytes e conteúdo coincidirem.

Também é possível gerar arquivos persistentes separados e comparar explicitamente:

```bash
python3 tools/study-converter/convert.py "$APKG" /tmp/enare-python.cps
"$PROJECT/tools/study-converter-dotnet/artifacts/validation-cli/osx-arm64/StudyConverter.Cli" \
  "$APKG" /tmp/enare-dotnet.cps
shasum -a 256 /tmp/enare-python.cps /tmp/enare-dotnet.cps
cmp -s /tmp/enare-python.cps /tmp/enare-dotnet.cps && echo "bytes idênticos" || echo "DIVERGÊNCIA"
build/host-tests/study_v1/study_package_check /tmp/enare-python.cps --open-only
build/host-tests/study_v1/study_package_check /tmp/enare-dotnet.cps --open-only
```

Resultado medido nesta sessão para o ENARE inteiro: mesmo SHA-256 `ec6c1bffc796623a4e957c13e55f9b654177304295a7eda882cb4da2e1948aaf`, 11 decks, 1.036 cards, 3 skips; `StudyPackageCheck` aceitou ambos com `OK 11 1036 3041508150`; harness retornou `PARITY`.

## 8. Preparar o CPS no SD e abrir Study

Com `/tmp/enare-python.cps` criado:

```bash
cd "$PROJECT"
python3 tools/simulator-test/prepare_sd.py \
  --epub "$PROJECT/test/epubs/test_reader_menu_progress.epub" \
  --cps /tmp/enare-python.cps --cps-name enare.cps
```

Confirme a estrutura Mac (o marcador oculto é só do helper):

```bash
find "$PROJECT/build/simulator-test-sd" -maxdepth 3 -type f -print
```

O mapa esperado é:

```text
build/simulator-test-sd/                 = raiz SD `/`
build/simulator-test-sd/books/test_reader_menu_progress.epub = `/books/test_reader_menu_progress.epub`
build/simulator-test-sd/study/enare.cps       = `/study/enare.cps`
```

Inicie o executável com o sandbox selecionado:

```bash
cd "$PROJECT"
export CROSSPOINT_SIM_SD="$PROJECT/build/simulator-test-sd"
"$PROJECT/.pio/build/simulator_x4/program"
```

Na tela Home, toque/clique **Study**. O catálogo lê os `.cps` diretamente em `/study/` (não pesquisa subpastas). Deve mostrar o cabeçalho `enare` e decks hierárquicos com contagem, incluindo `Preparatório R1 Vet - ENARE`, depois `> 2. Virologia (30)` na ordem da lista. Toque Virologia; abra o card. Na frente, escolha **Mostrar resposta**; no verso, experimente **Again**, **Hard**, **Good** e **Easy** em cards diferentes. Cada classificação avança para o próximo card elegível.

O conversor gera o package completo e o firmware permite selecionar um deck/subdeck a estudar; filtrar Virologia no conversor dá um segundo CPS independente com 30 cards.

## 9. O que Study grava e quando

Para `/study/enare.cps`, todos os caminhos abaixo são relativos à raiz SD (e portanto ficam sob `build/simulator-test-sd` no Mac):

```text
/study/enare.cps              conteúdo imutável; nunca recebe estado de review
/study/enare.state            estado de scheduling por card
/study/enare.session          checkpoint da sessão/resume
```

Os sufixos transitórios/recovery usam, quando existem, `.tmp` e `.bak` ao lado do arquivo canônico: por exemplo `enare.state.tmp`, `enare.state.bak`, `enare.session.tmp`, `enare.session.bak`; checkpoint corrompido/quarentenado também pode aparecer como `enare.session.bad`. Eles podem ser removidos após uma operação completa, então sua ausência em execução normal é esperada.

- **`.cps`**: Study V1 binário, header/tabelas/texto, CRC-32 do payload. O package identity usado para não associar dados antigos a outro package é tamanho + CRC; `.state` e `.session` validam CRC e quantidade de cards.
- **`.state`**: estado separado por card, dois slots alternados de 40 bytes/card depois de header de 32 bytes. Cada slot carrega geração, card ID, due day, intervalo, última revisão, repetições, lapses, status/rating e CRC. No primeiro open é inicializado por `.tmp`, validado e promovido para `.state`. Ao responder, firmware grava o slot inativo daquele card e atualiza seu CRC; o outro slot anterior ainda permite recuperar update interrompido.
- **`.session`**: checkpoint binário de 48 bytes: package CRC, deck ID, fase, face FRONT/BACK, índices/card atual e CRC. Escrever checkpoint usa `.tmp`, valida, rotaciona o anterior por `.bak` e promove. O fluxo de arquivo não é uma transação FAT multi-operação atômica.
- O checkpoint é salvo quando o card é carregado/aberto (face FRONT), ao mostrar resposta (face BACK), depois de uma avaliação e ao terminar uma sessão. Ao fechar a janela, o último checkpoint salvo permanece.
- Ao reabrir, o firmware exige CRC/card count/package/deck e card elegível. Se há card corrente, tenta voltar ao mesmo card e à mesma face. Depois de avaliar, o checkpoint aponta para o próximo card; o estado por card impede reavaliar como New. Sem checkpoint válido, recalcula próximo card por scheduling.
- Recuperação de estado: `.state` canônico válido ganha; senão backup válido é promovido; `.tmp` só é promovido para criação inicial quando canônico não existe. Arquivo incompatível/corrompido não deve ser substituído silenciosamente se progresso válido pode existir.
- Recuperação do checkpoint: válido canônico; senão valida temporário/backup segundo prioridade no código; inválido pode ser preservado em `.bad` antes de iniciar de novo. A implementação valida identidade/CRC antes de aceitar.

Veja o contrato detalhado em `docs/study-v1-format.md`. Para observar arquivos após algumas respostas:

```bash
find "$PROJECT/build/simulator-test-sd/study" -maxdepth 1 -type f -print
ls -l "$PROJECT/build/simulator-test-sd/study"
shasum -a 256 "$PROJECT/build/simulator-test-sd/study/enare.cps" \
  "$PROJECT/build/simulator-test-sd/study/enare.state" \
  "$PROJECT/build/simulator-test-sd/study/enare.session"
```

Não espere que `.tmp`/`.bak` continuem presentes depois do save concluído. Compare tamanhos/hash antes/depois; `.state` muda com resposta e `.session` muda com avanço/face.

## 10. Roteiro manual de checkpoint/resume

Para não herdar teste antigo, primeiro rode `python3 tools/simulator-test/prepare_sd.py --reset --cps /tmp/enare-python.cps --cps-name enare.cps` (esse comando deixa de incluir EPUB; Reader não é necessário aqui). Inicie o simulador:

1. Home → Study → `Preparatório R1 Vet - ENARE` → `2. Virologia`.
2. Abra o primeiro card. Antes de responder, a sessão está em FRONT.
3. Clique **Mostrar resposta**; agora está em BACK. Anote/hash `enare.session` antes/depois para observar atualização.
4. Clique **Hard**. Deve registrar review e abrir o próximo card em FRONT.
5. Feche a janela SDL normalmente.
6. Rode o mesmo comando de launch (mesmo `CROSSPOINT_SIM_SD`); Home → Study → mesmo pacote/subdeck.
7. Resultado esperado: retomar no próximo card, frente (a sessão avançou após Hard). O card avaliado está persistido no `.state`.

**Caso FRONT:** em sandbox resetada, abra pacote/subdeck e feche sem revelar; reabra e entre novamente. Deve retomar o mesmo primeiro card na frente. **Caso BACK:** abra o primeiro card, revele, feche sem classificar, reabra; deve voltar à resposta do mesmo card. Isso comprova que a face está codificada no checkpoint. Verifique sem editar os binários:

```bash
ls -l "$PROJECT/build/simulator-test-sd/study/enare.state" "$PROJECT/build/simulator-test-sd/study/enare.session"
shasum -a 256 "$PROJECT/build/simulator-test-sd/study/enare.state" "$PROJECT/build/simulator-test-sd/study/enare.session"
```

O CPS não deve mudar. CRC do checkpoint pode ser validado pelo `Study::decodeCheckpoint`/código host e pelo próprio firmware ao retomar; `StudyPackageCheck` valida o package CPS, não decodifica o `.session`.

## 11. Testar Reader e EPUB

O helper instala a fixture no caminho `/books/test_reader_menu_progress.epub`. Para um cenário só Reader:

```bash
python3 tools/simulator-test/prepare_sd.py --reset \
  --epub "$PROJECT/test/epubs/test_reader_menu_progress.epub"
cd "$PROJECT"
export CROSSPOINT_SIM_SD="$PROJECT/build/simulator-test-sd"
"$PROJECT/.pio/build/simulator_x4/program"
```

Na Home, abra **Library**/browser conforme a organização exibida e localize `test_reader_menu_progress.epub` (ou use **File Browser** → `books` se esse item estiver disponível na Home). Toque o EPUB. Na primeira abertura, espere indexação/layout/cache; navegue entre as páginas com setas/tap. Fixture tem capítulos curtos e serve para seleção de texto.

Os arquivos de cache/progresso do livro vivem sob `/.crosspoint/epub_<hash-do-caminho>/`; cache de página pode ser reconstruído. Não limpe essa pasta para testar highlights: ela guarda também `progress.bin`.

## 12. Highlights: seleção, lista, navegar, remover

No X4 Pro simulado, mouse gera touch. No corpo do texto:

1. Pressione o mouse sobre uma palavra até o long press entrar no modo seleção.
2. Sem soltar, arraste o ponto final até a última palavra; solte.
3. No popup **Select**, escolha **Done** para salvar; **Cancel** descarta seleção.
4. Abra o menu do Reader e **Highlights** (também exposto na superfície More/toolbar conforme tela). A lista apresenta trecho em ordem de leitura.
5. Toque numa linha para voltar ao trecho/capítulo ancorado.
6. Para remover, volte à página destacada, faça long press sobre o trecho existente; escolha **Delete** no popup. Confirme a exclusão. Em uma lista, long press de linha abre Open/Add note/Edit note/Cancel — remoção é pela seleção do trecho no Reader.

Persistência real confirmada no código: `/.crosspoint/highlights/highlight_<fnv1a64(bookPath)>.bin`. Como o livro deste roteiro é `/books/test_reader_menu_progress.epub`, o arquivo esperado é:

```text
/.crosspoint/highlights/highlight_9ad3b528d4b1c19f.bin
```

No Finder/Terminal, o caminho no Mac é:

```bash
ls -la "$PROJECT/build/simulator-test-sd/.crosspoint/highlights"
```

Árvore esperada depois de salvar:

```text
build/simulator-test-sd/
├── books/test_reader_menu_progress.epub
└── .crosspoint/highlights/highlight_9ad3b528d4b1c19f.bin
```

Sidecar binário: header magic `HLT1`, versão atual 2, path exato do EPUB, até 64 registros, IDs/índices/intervalos/quote, bytes de nota e CRC-32 IEEE final. Inteiros little-endian; texto UTF-8. Seleção é armazenada em offsets de codepoints visíveis da spine, não coordenada/página. Hash FNV-1a 64 do caminho determina nome, e o próprio path guardado rejeita colisão. Renomear/mover o livro muda a identidade; substituir conteúdo no mesmo caminho não fingerprinta o EPUB.

## 13. Notas vinculadas aos highlights

1. Abra **Highlights** no menu do Reader.
2. Faça long press na linha desejada (ou segure Confirm por 700 ms). Escolha **Add note**.
3. Digite no teclado da UI e confirme para salvar. Volte à lista; a linha deve mostrar indicador/subtítulo `Note`.
4. Faça long press na mesma linha e selecione **View / edit note** (texto UI pode aparecer como **Edit note**).
5. Para testar cancelamento, altere texto e use Back/cancel. Reabra a edição: conteúdo original deve permanecer.
6. Altere novamente e confirme; reinicie o simulador mantendo o mesmo SD; abra EPUB → Highlights → confirme que a nota continua.
7. Para apagar conteúdo da nota, edite e salve campo vazio; highlight fica, nota passa a não existir e indicador desaparece.

A nota é um campo do mesmo registro que o highlight (ID persistente), não um sidecar por nota. Limite atual: 512 **bytes UTF-8** por nota, não 512 caracteres; UTF-8 inválido, NUL e over-limit são rejeitados. Vazia (0 bytes) significa sem nota. Versão 1 antiga é lida como nota vazia; próximo save regrava coleção completa como v2. Um Cancel não chama persistência nem modifica modelo; save escreve coleção em `.tmp`, fecha, valida CRC, renomeia canônico para `.bak`, promove temporário e remove backup após sucesso. `.tmp` incompleto é ignorado; backup validado pode restaurar canônico ausente/corrompido. Renames separados não são atomicidade total do FAT.

**Inspeção segura:** não use editor binário e não modifique o sidecar. Confira nomes/tamanhos/hash e faça cópias antes de ensaios de recovery:

```bash
ls -l "$PROJECT/build/simulator-test-sd/.crosspoint/highlights"
shasum -a 256 "$PROJECT/build/simulator-test-sd/.crosspoint/highlights/highlight_9ad3b528d4b1c19f.bin"
python3 -c 'from pathlib import Path; p=Path("'"$PROJECT"'/build/simulator-test-sd/.crosspoint/highlights/highlight_9ad3b528d4b1c19f.bin"); b=p.read_bytes(); print("magic",b[:4],"version",b[4],"bytes",len(b),"trailer_crc_le",b[-4:].hex())'
```

O texto completo está dentro do registro binário length-prefixed. O dump acima é somente metadata; não prova CRC válido. Valide de forma segura reiniciando/reabrindo Highlights e conferindo texto/nota. Layout descrito em `docs/highlights-v1-format.md`.

## 14. Roteiro de persistência completo

Faça cada ensaio isoladamente na sandbox:

### Highlight

1. Reset e instale somente o EPUB; inicie, crie highlight, feche.
2. Confirme sidecar esperado e anote SHA/tamanho.
3. Reabra; confirme trecho na lista e navegação.
4. Remova highlight; feche/reabra; lista sem aquele trecho (se nenhum outro, mensagem `No highlights`/tradução equivalente). O arquivo pode continuar existindo com coleção vazia.

### Nota

1. Adicione nota; feche/reabra e confirme texto.
2. Edite e cancele; hash/conteúdo devem continuar iguais.
3. Edite e salve novo texto; sidecar hash deve mudar.
4. Reinicie e confirme o novo texto; salvar vazio deve remover indicador sem apagar highlight.

### Study

1. Reset e instale CPS; classifique card.
2. `.state` deve surgir/mudar, `.session` deve existir/mudar.
3. Reinicie no mesmo SD e confirme próximo card (ou mesma face se fechou antes da classificação).
4. CPS permanece idêntico ao hash de origem.

## 15. Recovery controlado de `.tmp`, `.bak` e corrupção

Faça apenas em cópia, com o simulador fechado. Primeiro estude um card e crie highlight/nota para que existam os arquivos canônicos; então crie o snapshot (destino não pode existir):

```bash
python3 tools/simulator-test/prepare_sd.py --snapshot \
  "$PROJECT/build/snapshots/study-good"
```

Antes de cada caso, restaure o baseline na sandbox isolada (isso apaga apenas o conteúdo marcado):

```bash
python3 tools/simulator-test/prepare_sd.py --reset --restore-snapshot "$PROJECT/build/snapshots/study-good"
```

O snapshot deve conter `.state` e `.session` válidos; para isso, primeiro estude ao menos um card. Para testar candidato temporário de checkpoint válido contra canônico corrompido:

```bash
python3 -c 'from pathlib import Path; import shutil; d=Path("'"$PROJECT"'/build/simulator-test-sd/study"); p=d/"enare.session"; shutil.copy2(p,d/"enare.session.tmp"); b=bytearray(p.read_bytes()); b[-1]^=1; p.write_bytes(b)'
```

Abra o mesmo package/deck: `.session.tmp` válido é promovido, retomando card/face do snapshot. Para backup, restaure snapshot e execute:

```bash
python3 -c 'from pathlib import Path; import shutil; d=Path("'"$PROJECT"'/build/simulator-test-sd/study"); p=d/"enare.session"; shutil.copy2(p,d/"enare.session.bak"); b=bytearray(p.read_bytes()); b[-1]^=1; p.write_bytes(b)'
```

O mesmo teste de backup funciona em `.state`/`.state.bak`. Para `.state.tmp` de criação inicial, restaure snapshot, copie o `.state` válido para `.state.tmp` e remova somente `.state` e `.state.bak` no sandbox; o temporário válido deve ser promovido. State não aceita `.tmp` se um canônico inválido ainda existe. Para erro sem candidato válido, restaure baseline e corrompa o canônico sem criar backup/temp; deve aparecer erro de status, sem inicialização por cima. Restaure snapshot antes de cada caso.

Highlights: restaure snapshot, copie o sidecar válido para `.bak`, corrompa canônico e reabra EPUB; backup com CRC válido deve restaurar o original. `.tmp` não é candidato para load de highlights; com canônico válido, um `.tmp` separado não deve mudar conteúdo exibido.

Resultados esperados pelo código atual:

- Package `.cps` válido: catálogo aparece.
- `.state` canônico válido: usado. Canônico ausente/corrompido + backup válido: backup promovido. `.tmp` pode recuperar criação inicial apenas se canônico ausente e sem backup; `.tmp` isolado inválido é removido durante inicialização. Um `.state` existente incompatível/corrompido sem candidato válido mostra erro de estado (`Os dados de estudo estão danificados ou são incompatíveis.`), não recria progresso por cima.
- `.session` canônico válido é escolhido primeiro; temporário/backup são aceitos após validação se o candidato prioritário não for válido. Um canônico inválido pode ser renomeado para `.bad`. Checkpoint inválido não zera `.state`; a sessão é reconstruída se nenhum checkpoint candidato válido for recuperável.
- Highlights: canônico válido é usado; senão backup válido pode restaurar. `.tmp` não é candidato de load e deve ser ignorado. Nunca deixe única cópia do sidecar em uma experiência de corrupção.

O firmware não oferece comando de UI para injetar cada falha. Esses cenários testam recuperação lógica de arquivos, não atomicidade FAT/queda elétrica. Não há injetor seguro de erro genérico de filesystem no simulador.

## 16. Casos de erro Study e mensagens

As mensagens em português vêm das strings atuais do firmware (podem mudar com idioma):

| Cenário                                             | Resultado esperado                                                                                                                                             |
| --------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `/study/` ausente                                   | Study tenta criar diretório; lista vazia: `Nenhum baralho encontrado. Copie um arquivo .cps para /study/. Para Anki, converta o APKG para .cps no computador.` |
| `/study/` vazio/sem `.cps`                          | Mesma mensagem de catálogo vazio.                                                                                                                              |
| CPS válido                                          | Arquivo/decks visíveis; sessão abre.                                                                                                                           |
| CPS inválido sozinho                                | `Nenhum pacote de estudo válido foi encontrado.`                                                                                                               |
| válido + inválido                                   | Deck válido segue disponível e UI também mostra aviso `Pacotes inválidos ignorados.`/tradução.                                                                 |
| deck sem cards                                      | ENARE tem `Default` com 0 cards; selecione-o. Ao abrir: `Este baralho não tem cartões.`                                                                        |
| package mudou, state package CRC/count incompatível | `O pacote mudou. O progresso salvo não corresponde a ele.`                                                                                                     |
| state danificado/incompatível                       | `Os dados de estudo estão danificados ou são incompatíveis.`                                                                                                   |
| erro de I/O ao abrir package                        | `Não foi possível abrir este pacote de estudo.` ou mensagem de leitura; varia pelo ponto da falha.                                                             |
| erro ao salvar resposta/checkpoint                  | `Não foi possível salvar sua resposta. Tente novamente.`                                                                                                       |

Para testar `/study/` vazio/inexistente: `python3 tools/simulator-test/prepare_sd.py --reset` e launch (helper deixa apenas marcador); Study cria `/study/` se necessário e mostra catálogo vazio. Para package inválido, crie cópia scratch corrompida sem alterar o CPS original:

```bash
python3 -c 'from pathlib import Path; b=bytearray(Path("/tmp/enare-python.cps").read_bytes()); b[-1]^=1; Path("/tmp/enare-invalid.cps").write_bytes(b)'
python3 tools/simulator-test/prepare_sd.py --reset --cps /tmp/enare-invalid.cps --cps-name invalid.cps
build/host-tests/study_v1/study_package_check "$PROJECT/build/simulator-test-sd/study/invalid.cps" --open-only
```

Launch para inválido sozinho deve mostrar `Nenhum pacote de estudo válido foi encontrado.` Para válido + inválido, restaure ambos só no sandbox:

```bash
python3 tools/simulator-test/prepare_sd.py --reset --cps /tmp/enare-python.cps --cps-name enare.cps
python3 -c 'from pathlib import Path; import shutil; d=Path("'"$PROJECT"'/build/simulator-test-sd/study"); shutil.copy2("/tmp/enare-invalid.cps",d/"invalid.cps")'
```

Study deve manter os decks válidos e indicar que o package inválido foi ignorado. Para incompatibilidade state/package, estude um card no CPS ENARE, feche o app, depois substitua `/study/enare.cps` por `/tmp/enare-virologia-python.cps` **no sandbox**, mantendo `/study/enare.state`; ao abrir, deve indicar package alterado/incompatibilidade, pois CRC/card count não correspondem. O deck `Default` sem cards no ENARE cobre deck vazio. Não existe injetor seguro de falha genérica de filesystem no simulador.

Para validar erro de parser de forma determinística sem UI, rode:

```bash
build/host-tests/study_v1/study_package_check /tmp/arquivo-invalido.cps --open-only
```

`StudyPackageCheck` responde no Terminal; mensagem visual exata depende do tipo de erro e locale. Erro de filesystem SD real e comportamento de energia durante escrita continuam precisando de teste controlado/hardware.

## 17. A/B Python × .NET no simulador

Os dois conversores produziram bytes idênticos no ENARE. Mesmo assim, isole o estado entre rodadas:

**Rodada A — Python**

```bash
python3 tools/study-converter/convert.py "$APKG" /tmp/enare-python.cps
python3 tools/simulator-test/prepare_sd.py --reset --cps /tmp/enare-python.cps --cps-name enare.cps
```

Inicie com `CROSSPOINT_SIM_SD="$PROJECT/build/simulator-test-sd"`, entre no subdeck Virologia, responda alguns cards e anote a ordem/comportamento.

**Rodada B — .NET**

Feche o simulador primeiro; `.state` e `.session` antigos estão vinculados ao mesmo CPS e, sendo bytes idênticos, seriam aceitos. O reset controlado é obrigatório para garantir estado inicial igual:

```bash
"$PROJECT/tools/study-converter-dotnet/artifacts/validation-cli/osx-arm64/StudyConverter.Cli" \
  "$APKG" /tmp/enare-dotnet.cps
python3 tools/simulator-test/prepare_sd.py --reset --cps /tmp/enare-dotnet.cps --cps-name enare.cps
```

Repita o mesmo subdeck/respostas. CPS byte-identical implica mesmos decks, cards e sequência inicial no mesmo relógio local; o simulador usa seu host clock (UTC day) e não prova agendamento real de longa duração.

## 18. Reader + Study no mesmo SD

```bash
python3 tools/simulator-test/prepare_sd.py --reset \
  --epub "$PROJECT/test/epubs/test_reader_menu_progress.epub" \
  --cps /tmp/enare-python.cps --cps-name enare.cps
```

1. Inicie simulador e abra Reader → EPUB; leia trecho.
2. Crie highlight e adicione nota.
3. Volte Home; Study → package → Virologia; revele e classifique um card.
4. Volte Home e reabra Reader; confira highlight e nota.
5. Feche/reabra o processo sem `--reset`; confirme Reader/nota e retorne Study para verificar estado e checkpoint.

Estrutura Mac resultante contém `books/test_reader_menu_progress.epub`, `study/enare.cps`, `study/enare.state`, `study/enare.session`, `/.crosspoint/highlights/highlight_9ad3b528d4b1c19f.bin` e cache/progresso EPUB sob `/.crosspoint/epub_<hash>/`. Os prefixos de state/checkpoint são basename do CPS, não ficam dentro do cache do livro.

## 19. Troubleshooting

- **Janela SDL não abre:** confira `test -x .pio/build/simulator_x4/program`, refaça `pio run -c platformio.sim.ini -e simulator_x4`, SDL2 e rode a partir de Terminal para logs.
- **Study não acha CPS:** `echo "$CROSSPOINT_SIM_SD"`; confira literal `<SD>/study/enare.cps` e extensão minúscula/maiúscula `.cps`. O catálogo não percorre subdiretórios.
- **Subdeck Python “não encontrado”:** copie o nome de `--inspect`; nome é `Preparatório R1 Vet - ENARE::2. Virologia` (não `Veterinária::Virologia`).
- **Comparação não roda:** verifique `$PROJECT/tools/study-converter-dotnet/src/StudyConverter.Cli/bin/Release/net10.0/StudyConverter.Cli.dll`; rode `dotnet build tools/study-converter-dotnet/StudyConverter.sln -c Release` se SDK 10 instalado. O published CLI osx-arm64 já está disponível neste checkout.
- **Reader não lista livro:** confirme `books/test_reader_menu_progress.epub`, volte Home/Library e verifique fixture. First open pode indexar.
- **Cache desatualizado:** não apague `/.crosspoint` indiscriminadamente; preserve `progress.bin` e sidecar highlights. Reproduza primeiro com sandbox isolado/resetável.
- **Abrir app .NET bloqueado:** localize o bundle correto e confirme abrir pelo menu contextual do Finder. O app não depende de runtime .NET instalado.

## 20. Verificações CLI úteis

Build Checker e testes host:

```bash
cmake -S test -B build/host-tests
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

O checker de pacote usado acima é `build/host-tests/study_v1/study_package_check`. Para executar uma fixture CPS emitida pelo teste em teste end-to-end, consulte as opções de `python3 test/study_converter/test_converter.py --help`.

## 21. Limites: o que ainda requer X4 Pro físico

O simulador marca a placa como `xteink_x4_pro` e expõe touch sintético, tela SDL e um filesystem POSIX, mas não valida:

- controlador/sensor GT911 real, precisão/ruído/calibração, latência e gestos sob dedo;
- botões físicos, Home capacitivo físico, bounce e comportamento elétrico;
- SDMMC real, cartão FAT, compatibilidade, velocidade, remoção inesperada, corrupção por queda de energia ou limites de filesystem do cartão;
- USB MSC, enumeração USB, host macOS, montagem/ejeção do cartão e concorrência entre firmware/computador;
- painel e-paper real, waveform, ghosting, contraste, refresh parcial/total, inversão, orientação física e defeitos de lote;
- backlight/frontlight elétrico, PWM, temperatura/cor real;
- ESP32-S3 em firmware: heap/PSRAM, stack, flash, timing, performance, watchdog, consumo e aquecimento;
- RTC e timekeeping/calendário com hardware real e consequências de agendamento em dias distintos;
- ciclo de deep sleep/boot elétrico, wake sources e persistência sob queda abrupta de energia.

`simulator_x4` **PASS** significa que o binário host do perfil X4 Pro compilou e executou sobre stubs SDL/filesystem; não significa hardware PASS. Nesta fase rodei build, inicialização → Home → encerramento e checker, mas não automatizei os cliques do Reader/Study. Há registro de smoke integrado anterior deste checkout em `docs/STUDY_V1_STATUS.md`: relata Reader/Highlights/Notes (inclusive cancelar/salvar/reiniciar/remover) junto de Study Virologia, Hard e resume. Use os roteiros acima para repetir pessoalmente e verificar a UX atual; a validação final continua exigindo aparelho real.
