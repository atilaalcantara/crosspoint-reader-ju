# Study Converter Desktop (.NET)

Este diretório contém a primeira implementação exploratória do aplicativo desktop Study Converter. O objetivo final é converter pacotes `.apkg` localmente para Study V1 `.cps`, sem exigir runtimes ou ferramentas do sistema à pessoa usuária.

## Estado desta implementação

O harness diferencial agora confirma igualdade byte-a-byte do CPS, registros de deck/card e contagem de skips para a fixture existente e para o APKG ENARE local. Na fixture ambos geram 7 cards e 3 skips; no ENARE ambos geram 1.036 cards de 1.039 e 3 skips. Os dois CPS passam pelo `StudyPackageCheck`. O pacote ENARE é privado/local e não foi adicionado ao repositório.

A UI macOS foi aberta a partir do bundle self-contained e a tela inicial foi revisada/corrigida para contraste legível. A captura está em [`artifacts/screenshots/macOS-initial.png`](artifacts/screenshots/macOS-initial.png). O CLI macOS self-contained converteu o ENARE e seu CPS passou no `StudyPackageCheck` com o mesmo SHA-256 do Python. O fluxo visual de abrir APKG, escolher subtree, salvar e rever o sucesso/warnings ainda não foi percorrido; Windows/Linux foram cross-published, sem execução nos SOs de destino.

A coleção Zstandard agora tem limite incremental de 128 MiB de saída. O conversor lê manifestos de mídia JSON (APKG legado) e protobuf Zstandard (APKG v3) com limite incremental de 4 MiB. Somente imagens referenciadas nos cards convertidos são decodificadas; imagens ausentes, inválidas e não suportadas geram avisos agrupados. Nenhuma mídia é copiada para pacotes só de texto.

O contrato de referência é [`../study-converter/convert.py`](../study-converter/convert.py) e [`../../docs/study-v1-format.md`](../../docs/study-v1-format.md). A implementação do firmware e as fixtures existentes não foram modificadas.

## Arquitetura e dependências

- **.NET 10**, Avalonia 12 para UI multiplataforma.
- `Microsoft.Data.Sqlite` para acesso read-only; SQLite nativo é incluído pelo pacote (`SQLitePCLRaw.lib.e_sqlite3` 2.1.13). `dotnet list package --vulnerable --include-transitive` não reportou advisories no restore verificado.
- `ZstdSharp.Port` para descompressão incremental e limitada de coleção e manifesto APKG v3.
- `SixLabors.ImageSharp` para decodificar, redimensionar e codificar imagens em PNG multiplataforma.
- Sem chamadas de rede, login ou telemetria implementados.

## Imagens Study (contrato com o firmware)

Para saída `/study/foo.cps`, quando há imagens suportadas referenciadas pelos cards selecionados, o conversor cria `/study/foo.media/index.bin` e os arquivos PNG no mesmo diretório. Copie **o `.cps` e a pasta `.media` juntos** para o SD. Conversões sem imagens não criam a pasta (e removem uma pasta obsoleta da mesma saída). O layout binário do `.cps` V1 permanece inalterado, inclusive os bytes de um pacote só de texto.

`index.bin` tem cabeçalho de 20 bytes: ASCII `CPSMEDIA` (offset 0, 8 bytes); versão `uint16 LE = 1` (offset 8); reservado `uint16 = 0` (offset 10); CRC-32 do payload `.cps` copiado do cabeçalho `.cps` offset 36 (`uint32 LE`, offset 12); quantidade de registros `uint32 LE` (offset 16). Cada registro, ordenado por ID do card, face (0 frente, 1 verso) e ordem da imagem na face, tem `uint64 LE cardId`, `uint8 face`, `uint8 reservado=0`, `uint16 LE tamanho do nome` (1..64) e os bytes ASCII do nome, sem terminador. Máximo de quatro imagens válidas por face. O arquivo de imagem chama-se `<primeiros 32 caracteres hex minúsculos do SHA-256 dos bytes PNG>.png`; imagens iguais são armazenadas uma vez, embora possam ter múltiplos registros.

Os pixels são ajustados proporcionalmente a no máximo 800×480, compostos sobre branco, convertidos a cinza e salvos em PNG. A entrada de cada imagem é limitada a 16 MiB descompactados do ZIP e 16 milhões de pixels antes do resize. As referências vêm de `src` em tags `<img>` após renderizar os templates; o texto/alt continua seguindo a conversão V1. Não há suporte a SVG, áudio ou imagens externas. Avisos incluem exemplos de nomes de mídia e IDs dos cards; uma referência sem entrada correspondente no manifesto ou membro numérico no ZIP aparece como `mídia ausente`.

## Desenvolvimento

Requer .NET SDK 10 e acesso a NuGet:

```sh
dotnet restore StudyConverter.sln
dotnet build StudyConverter.sln -c Release
dotnet run --project tests/StudyConverter.Smoke -c Release
```

Após o build, rode o diferencial card a card e byte a byte:

```sh
python3 ../../test/study_converter/differential_dotnet.py
python3 ../../test/study_converter/differential_dotnet.py --fixture modern
python3 ../../test/study_converter/differential_dotnet.py --fixture modern-missing-meta
python3 ../../test/study_converter/differential_dotnet.py "/caminho/para/deck.apkg"
```

O segundo comando aceita o APKG ENARE local, se disponível. O harness produz ambos CPS em diretório temporário, lista IDs ausentes/extras e frentes/versos divergentes, compara SHA-256/bytes e invoca `StudyPackageCheck` quando o binário host está presente.

## Publicação self-contained

Do diretório `tools/study-converter-dotnet`:

```sh
dotnet publish src/StudyConverter.Desktop/StudyConverter.Desktop.csproj -c Release -r osx-arm64 --self-contained true -o artifacts/osx-arm64
dotnet publish src/StudyConverter.Desktop/StudyConverter.Desktop.csproj -c Release -r win-x64 --self-contained true -o artifacts/win-x64
dotnet publish src/StudyConverter.Desktop/StudyConverter.Desktop.csproj -c Release -r linux-x64 --self-contained true -o artifacts/linux-x64
dotnet publish src/StudyConverter.Desktop/StudyConverter.Desktop.csproj -c Release -r linux-arm64 --self-contained true -o artifacts/linux-arm64
```

Builds geradas localmente neste workspace:

- `artifacts/StudyConverter.app` (bundle macOS Apple Silicon; aberto no host para validar launch)
- `artifacts/win-x64/StudyConverter.Desktop.exe`
- `artifacts/linux-x64/StudyConverter.Desktop`
- `artifacts/linux-arm64/StudyConverter.Desktop`
- `artifacts/validation-cli/osx-arm64/StudyConverter.Cli` (smoke self-contained, ferramenta de validação)

| RID | Publish | Runtime no alvo | Evidência |
|---|---|---|---|
| `osx-arm64` | Sim | Parcial | bundle abriu; CLI self-contained converteu ENARE; captura inicial disponível |
| `win-x64` | Sim | Pendente | PE x64 e assets nativos inspecionados; requer Windows real |
| `linux-x64` | Sim | Pendente | ELF x64 e assets nativos inspecionados; requer Linux real |
| `linux-arm64` | Sim | Pendente | ELF AArch64 e assets nativos inspecionados; requer Linux ARM64 real |

As builds por SO não foram executadas no Windows/Linux de destino. A inspeção de formatos/arquivos não substitui runtime validation nesses sistemas.
