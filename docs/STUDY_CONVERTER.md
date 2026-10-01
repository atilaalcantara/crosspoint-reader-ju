# Conversor local Anki `.apkg` → Study V1

## Uso

Na raiz do repositório:

```bash
python3 tools/study-converter/convert.py input.apkg output.cps
python3 tools/study-converter/convert.py input.apkg output.cps --deck "Veterinária::Virologia"
python3 tools/study-converter/convert.py --help
```

O CLI não exige dependências Python externas, site, AnkiWeb ou login. A opção `--deck` exige o nome completo encontrado no pacote e converte esse deck e seus descendentes. Sem filtro, inclui decks normais do pacote. O relatório lista decks/cards convertidos, cards ignorados por razão, Cloze, manifesto/referências de mídia e caminho de saída.

## Recomendações de exportação

No Anki Desktop, use **File → Export → Anki Deck Package (`.apkg`)** para o deck/subtree que deseja levar ao X4 Pro.

- **Include Scheduling Information:** desmarcado. O Study V1 guarda seu próprio estado; não importa histórico nem agenda do Anki.
- **Include Media:** opcional. Se marcado, o conversor identifica referências e reporta o que foi omitido; Study V1 ainda é somente texto.
- Para o APKG v3 padrão do Anki, deixe **Support older Anki versions (slower/larger files)** desmarcado e tenha `zstd` no `PATH`. Marque essa opção se não puder instalar `zstd`; o Anki exportará o SQLite legado, lido somente com a biblioteca padrão do Python.
- Evite exportar a coleção inteira (`.colpkg`, ou `collection.apkg`) sem `--deck`. Esse conversor destina-se a um deck package, não faz backup da coleção Anki.

Depois da conversão, copie `output.cps` diretamente para `/study/` no cartão SD. O firmware cria essa pasta ao abrir Study pela primeira vez e cria `<basename>.state` e `<basename>.session` ao abrir o pacote. Veja [`docs/V1_STATUS.md`](V1_STATUS.md) para o fluxo completo por USB MSC ou File Transfer via Wi-Fi.

## Instalar no X4 Pro

O X4 Pro expõe o cartão SD inteiro no modo USB Drive; o computador não recebe
uma partição ou pasta de importação separada.

1. Abra **Home → File Transfer → USB Drive** e conecte o aparelho ao computador.
2. Na raiz do cartão, crie `/study/` se ainda não existir e copie o `.cps` para
   dentro dela. Ejete o cartão com segurança no sistema operacional.
3. Quando o firmware voltar à Home, abra **Study**. A lista varre `/study/` ao
   entrar; basta sair e reabrir Study para atualizar o catálogo depois de outra
   cópia.

Também é possível usar **Home → File Transfer → Join Network/Create Hotspot**,
abrir o URL mostrado no aparelho e usar **Files** para criar `/study/` e enviar
o `.cps`. Encerre File Transfer e entre em Study. O guia de transferência e o
File Manager estão em [`docs/webserver.md`](webserver.md). Não é preciso um
botão de atualização nem sincronização em segundo plano.

## O que há dentro de `.apkg`

O manual do Anki descreve `.apkg` como pacote ZIP com cards, notes, note types e, opcionalmente, mídia. Exportações antigas podem conter `collection.anki2` ou SQLite não comprimido `collection.anki21`; exportações modernas v3 contêm `collection.anki21b` e manifesto de mídia comprimidos com Zstandard. O conversor aceita as três variantes. Para v3, requer o executável `zstd` disponível no `PATH`; os formatos antigos continuam sem dependências externas além de Python.

O conversor seleciona a coleção pelo `meta` do pacote: em v3, `collection.anki21b` tem precedência obrigatória sobre `collection.anki2`, que pode ser uma coleção-iscas válida em SQLite, sem o conteúdo exportado. No banco legado, JSON `col.models`/`col.decks` descreve note types/templates/decks. No banco normalizado v3, `notetypes`, `fields`, `templates` e `decks` carregam esses metadados; configurações de templates e tipo são decodificadas como protobuf. Os nomes de subdecks normalizados pelo Anki usam separador de unidade e são mapeados para `::`.

Em v3, o manifesto `media` também é protobuf comprimido com Zstandard; o conversor conta as entradas e não descompacta nem copia os objetos de mídia. O SQLite mantém relações distintas: `notes` contém campos e note-type ID; `cards` contém um registro por card, com `nid`, deck ID atual, `ord` e, quando existente, deck original. Portanto, percorre cards existentes e renderiza o template escolhido pelo `ord`; não presume `1 note = 1 card`, nem tenta regenerar coleção/scheduler.

## Subconjunto de templates e Cloze

O conversor suporta substituições `{{Field}}`, filtro `{{text:Field}}`, `{{FrontSide}}` no verso e condicionais simples/aninhadas `{{#Field}}...{{/Field}}` / `{{^Field}}...{{/Field}}`. Templates são expandidos em texto no desktop e depois o HTML vira UTF-8 plain text para o pacote; o firmware não recebe HTML/CSS executável.

Cloze suporta campos com `{{cN::texto}}`, dica opcional `{{cN::texto::dica}}`, vários números no mesmo texto e condicionais simples `{{#cN}}`. Cada card usa `cards.ord` para selecionar a exclusão. Cloze aninhado, filtros avançados e sintaxe ambígua são reportados e o card afetado não é escrito; demais cards continuam conversíveis. Isso evita publicar silenciosamente uma pergunta/resposta incorreta.

Imagens são substituídas pelo atributo `alt` ou `[image omitted]`; áudio/vídeo e `[sound:...]` são removidos do texto e contados no relatório. Blocos LaTeX conhecidos viram `[LaTeX omitted]` e entram no relatório. CSS, scripts, campos especiais não implementados, JavaScript e add-ons não são executados. Cartões com frente vazia ou faces acima dos limites de Study V1 são explicitamente ignorados e contados.

## Determinismo, atomicidade e segurança

- Deck IDs `.cps` são IDs locais atribuídos pela ordenação do nome completo de deck. Relações `parentDeckId` são persistidas.
- Card IDs são os IDs numéricos existentes na tabela `cards`, ordenados crescentemente; IDs duplicados ou não positivos são erro.
- Uma entrada/opção idêntica gera `.cps` idêntico: não há timestamp, UUID nem hash dependente do runtime.
- O banco é extraído em streaming para temporário com limites; a saída Zstandard também é limitada a 128 MiB para o banco e 4 MiB para o manifesto. ZIP paths nunca são extraídos ao filesystem. SQLite abre read-only. Limites de entries/tamanho, checks de schema e erros ZIP/SQLite/protobuf/Zstandard viram erro CLI legível.
- A saída é montada num arquivo temporário no diretório destino, flush/fsync, validada estruturalmente e substituída por `os.replace`. Falha mantém uma saída anterior intacta e remove temporários quando seguro.
- O próprio Python faz uma validação estrutural/CRC do `.cps`; os testes de integração chamam também `StudyPackage::open` no parser C++ real, como verificação independente do formato.

## Testes

```bash
python3 test/study_converter/test_converter.py --checker build/host-tests/study_v1/study_package_check
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

As fixtures criam SQLite/APKG controlados para Basic, múltiplos templates/cards por note, subdecks, Cloze, HTML/entidades, mídia, Unicode, frente vazia, pacote vazio/inválido, filtro de deck e determinismo. Uma fixture Anki v3 verifica a precedência sobre a coleção-iscas, o schema normalizado, os templates protobuf e o manifesto de mídia. CTest integra conversão Python com o executável host que abre o `.cps` via `StudyPackage`.

Na validação real, um `.apkg` v3 com ambos os membros de coleção confirmou que não se pode escolher `collection.anki2` apenas por ser SQLite válido: essa entrada continha somente a coleção-iscas. A coleção exportada estava no membro Zstandard `collection.anki21b`.

## Referências Anki

- [Manual: Exporting](https://docs.ankiweb.net/exporting.html) — `.apkg`, `.colpkg`, opções de mídia/agendamento/compatibilidade.
- [Manual: Field Replacements](https://docs.ankiweb.net/templates/fields.html) — fields, `FrontSide`, HTML stripping, mídia e filtros.
- [Manual: Card Generation](https://docs.ankiweb.net/templates/generation.html) — criação de múltiplos cards e condicionais.
- [Manual: Cloze Deletion](https://docs.ankiweb.net/editing.html#cloze-deletion) — ordinais, hints, exclusões múltiplas e casos aninhados.
- [Código-fonte Anki](https://github.com/ankitects/anki) — implementação/schema podem evoluir; formatos fora do subconjunto são recusados ou reportados, não interpretados por heurística silenciosa.
