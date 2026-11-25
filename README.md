# tex2epub

A lightweight, standalone C++17 utility for turning LaTeX book sources into EPUB 3 packages without Python, Pandoc, or heavy runtime dependencies—relying only on the standard `zip` tool.

Given a single root `.tex` file (`./tex2epub path/to/book.tex`), it recursively expands `\input` files, extracts the bibliography before chapter parsing, strips common typesetting and table-of-contents macros (`\markboth`, `\addcontentsline`, `\tableofcontents`, page breaks, and spacing commands), converts basic inline formatting, block quotations, images, `\href` and `\cite` links, typographic dashes/quotes, and footnotes, and writes both the EPUB directory tree and the packaged `.epub` archive right next to the source file.

## Features

- **Single-command workflow:** Run `./tex2epub <input.tex>` to generate `<stem>/` and `<stem>.epub` alongside the source `.tex` file.
- **Recursive `\input` expansion:** Resolves nested `.tex` files relative to each file's directory with cycle detection.
- **Structure & navigation:** Parses `\title`, `\author`, `\date`, `\part`, and `\chapter` into XHTML chapters, a visible `toc.xhtml` page in the spine, EPUB 3 `nav.xhtml`, and a legacy-compatible `toc.ncx`.
- **Bibliography & citations:** Pre-parses `\begin{thebibliography}...\end{thebibliography}` (including `\input`-included `\bibitem{key}` entries) into a dedicated `bibliography.xhtml` page at the end of the book, turning `\cite{key}` (and optional `\cite[note]{key}`) across all chapters into numbered cross-references.
- **Block quotations:** Renders `\begin{quotation}...\end{quotation}` environments as indented `<blockquote class="quotation">` blocks at full body font size.
- **Images & captions:** Embeds `\includegraphics` assets (JPEG, PNG, WebP, SVG, GIF) into `OEBPS/images/` with case-insensitive filename fallback, rendering `\begin{center} \includegraphics{...} caption \end{center}` blocks as `<figure>` elements with `<figcaption>`.
- **Footnotes & links:** Renders `\footnote{...}` with `epub:type` cross-references and backlinks, and converts `\href{url}{label}` into XHTML `<a>` links.
- **Deterministic metadata:** Generates a 64-bit FNV-1a `urn:book:...` identifier from author and title, and sets `dcterms:modified` to the current UTC timestamp in ISO 8601 format.
- **Spec-compliant packaging:** Invokes `zip` to store `mimetype` uncompressed as the first entry, followed by `META-INF` and `OEBPS`.

## Credits

Vibebcoded by Peter Semiletov with Gemini
