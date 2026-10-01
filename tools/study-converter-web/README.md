# Conversor Study no navegador

Página estática, sem backend ou upload: `.apkg` → Study V1 `.cps` no próprio navegador. Suporta exportações legadas SQLite e Anki v3 (`collection.anki21b`/Zstandard), templates Basic e Cloze compatíveis, subdecks e relatório de cartões ignorados. Imagens compatíveis são convertidas localmente para PNG cinza e empacotadas com o `.cps` e seu índice `.media/index.bin` em um ZIP. Áudio e LaTeX são omitidos.

```sh
cd tools/study-converter-web
npm ci
npm run dev
npm test
npm run build
```

Publique **todo o conteúdo** de `dist/` em qualquer hospedagem estática; `--base=./` permite servir em subdiretórios. O build inclui SQLite WASM e o descompressor Zstandard, sem serviços externos e sem requisições de rede: a interface usa apenas fontes do sistema (SF Pro via `system-ui`/`-apple-system`), seguindo o DESIGN.md de referência estilo Apple — nav preta fixa, sub-nav com blur, tiles full-bleed alternando branco/parchment/preto e Action Blue (`#0066cc`) como único cor de ação, responsiva de 360 px a desktop (alvos de toque ≥ 44 px, `prefers-reduced-motion` respeitado). Não publique o servidor `vite dev` como serviço de produção.

O usuário exporta um pacote de deck `.apkg` do Anki, escolhe um deck ou todos na página e baixa o `.cps` (ou o ZIP quando houver imagens). Copie o `.cps` como arquivo direto em `/study/` na raiz do cartão SD do X4 Pro; ao receber ZIP, extraia **o `.cps` e a pasta `<nome>.media/`** nessa mesma pasta (Home → File Transfer → USB Drive, ou via Files no File Transfer Wi-Fi). A página não lê `.colpkg` nem importa agendamento do Anki. Para pacotes grandes, o navegador precisa de RAM suficiente para o ZIP, SQLite e saída simultâneos; o limite de entrada é 512 MiB.
