## Dependências

```bash
brew install gtk+3
brew install ghostscript
```

## Uso rápido (Linha de comando)

```bash
./pdfcompressor <arquivo.pdf> [--level low|medium|high]
```

* `<arquivo.pdf>` – caminho para o PDF que será comprimido.
* `--level` – controla a qualidade da compressão:
  * `low`    – maior compressão, qualidade mínima (`/screen`).
  * `medium` – compressão padrão (`/ebook`).
  * `high`   – compressão mínima, melhor qualidade (`/prepress`).
  * Se omitido, o nível padrão é **medium**.

O arquivo resultante será salvo em **$HOME/pdfcompressor/compressed** (criado
automaticamente na primeira execução). Cada compressão gera um arquivo com o
sufixo `_compressed.pdf` e, se já existir um arquivo com o mesmo nome, um
contador sequencial é adicionado (`_compressed_1.pdf`, `_compressed_2.pdf`,
etc.).

### Depuração de erros de compressão

Se a compressão falhar, a aplicação mostrará a mensagem retornada pelo Ghostscript, por exemplo:

```
Falha ao comprimir: Error: /cannot open %stdin for reading
```

Isso indica o motivo exato (arquivo corrompido, falta de permissão, etc.).
Com a mensagem detalhada, basta corrigir o problema apontado ou garantir que o PDF seja válido e legível.

## Deploy (GUI)

```bash
cd pdfcompressor
./run.sh
```

ou, usando o Makefile:

```bash
make app
open "PDF Compressor.app"
```
