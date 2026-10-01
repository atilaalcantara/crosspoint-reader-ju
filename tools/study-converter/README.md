# Conversor Study V1

Converte offline um pacote de deck `.apkg` do Anki Desktop para um pacote textual Study V1 `.cps`, aceito pelo firmware CrossPoint. Requer Python 3.10+; APKGs modernos v3 também requerem o executável `zstd` no `PATH`. Pacotes SQLite antigos continuam usando somente a biblioteca padrão Python.

```bash
python3 tools/study-converter/convert.py input.apkg output.cps
python3 tools/study-converter/convert.py input.apkg output.cps --deck "Veterinária::Virologia"
python3 tools/study-converter/convert.py --help
```

`--deck` recebe o nome completo do deck e inclui seus descendentes. Sem a opção, converte os decks normais incluídos no pacote. Um arquivo `.cps` é escrito por temporário validado e substituição atômica no mesmo diretório; em erro, a saída anterior permanece intacta.

## Exportação recomendada no Anki Desktop

1. Selecione o deck desejado (inclusive um deck pai para levar seus subdecks) e use **File → Export**.
2. Escolha **Anki Deck Package (`.apkg`)**; não use `.colpkg`/`collection.apkg` como export sem deck selecionado.
3. Desmarque **Include Scheduling Information**: Study mantém agendamento próprio e não importa histórico do Anki.
4. Marque **Include Media** se quiser que o conversor possa detectar referências e reportá-las. A mídia ainda não é enviada ao firmware Study V1.
5. Para APKGs modernos padrão (v3), mantenha **Support older Anki versions (slower/larger files)** desmarcado; o conversor lê `collection.anki21b` e o manifesto compactados se `zstd` estiver instalado. Marcar essa opção gera um SQLite antigo, legível sem Zstandard.

O conversor não modifica nem importa nada de volta à coleção Anki. No X4 Pro, use **Home → File Transfer → USB Drive** para montar o próprio SD, crie `/study/` na raiz se necessário, copie o `.cps` e ejete a unidade com segurança. Alternativamente, use **Home → File Transfer → Join Network/Create Hotspot** e a página **Files** para criar a pasta e enviar o arquivo. Depois, entre em **Study**: o catálogo é relido a cada entrada; sair e abrir Study novamente atualiza a lista após uma cópia. A pasta também é criada automaticamente quando Study é aberto e ainda não existe. O fluxo completo e o checklist físico estão em [`docs/V1_STATUS.md`](../../docs/V1_STATUS.md).

## Conteúdo suportado e limitações

- Abre `.apkg` ZIP legado com SQLite `collection.anki2`/`collection.anki21` e APKG v3 moderno com `collection.anki21b` e manifesto de mídia Zstandard. Em v3, a versão do pacote determina a seleção da coleção moderna; `collection.anki2` pode ser uma coleção-iscas válida, sem os dados exportados. O schema normalizado de note types, fields, templates e decks é convertido ao modelo interno do conversor.
- Lê a tabela de **cards**, não presume uma nota por card. A frente/verso são renderizados conforme o template e `ord` do card; IDs originais de card são mantidos.
- Templates: substituições de campo `{{Field}}`, `{{text:Field}}`, `{{FrontSide}}` no verso e condicionais `{{#Field}}`/`{{^Field}}` simples ou aninhadas. Filtros desconhecidos, campos/modelos ausentes e templates fora do subconjunto fazem o card ser explicitamente ignorado e contado no resumo — nunca são interpretados como HTML no firmware.
- Cloze: suporta sintaxe básica não aninhada `{{cN::texto}}`, dica `{{cN::texto::dica}}`, vários números no mesmo texto e condicionais `{{#cN}}`. O card correspondente esconde o cloze da frente e o revela no verso. Clozes aninhados e sobreposições não suportados são reportados e os cards afetados são ignorados.
- HTML é achatado para texto: `<br>` e elementos de bloco preservam quebras, entidades são decodificadas uma vez, scripts/styles são descartados e whitespace horizontal é normalizado. CSS, JavaScript, filtros avançados e lógica de add-ons não são executados. Blocos `[latex]...[/latex]`, `[$]...[/]` e `[$$]...[/$$]` viram `[LaTeX omitted]` e são contados no resumo.
- Detecta mídia pelo manifesto e pelo HTML renderizado. Imagens viram seu `alt` ou `[image omitted]`; referências `[sound:...]` e conteúdo de áudio/vídeo não ficam como lixo literal. Mídia não é copiada nem tocada pelo Study V1.
- Para APKG v3, o utilitário `zstd` precisa estar disponível no `PATH`; dados descompactados são limitados a 128 MiB para SQLite e 4 MiB para o manifesto. Conteúdo de mídia não é descompactado.
- Limites do formato (`docs/study-v1-format.md`): 1.024 decks, 100.000 cards, nome até 96 bytes e frente/verso até 4 KiB cada. Cards com frente vazia, face acima do limite ou template não suportado são omitidos com resumo/aviso.

IDs de deck são atribuídos determinística e localmente ao pacote pela ordenação de nomes completos de deck; pai é registrado por ID. Card IDs Anki são preservados, ordenados crescentemente e namespaced pelo arquivo `.cps` no firmware. A mesma entrada e opções produzem bytes idênticos, sem timestamp nem hash aleatório.
