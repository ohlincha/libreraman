#!/bin/sh
# Makes docs/LibreRamen-QuickStart.pdf and docs/LibreRamen-Manual.pdf from docs/QUICKSTART.md and docs/MANUAL.md:
# pandoc → one self-contained HTML (pictures embedded, docs/manual.css) → Chrome headless → PDF.
# Needs pandoc and google-chrome (or chromium; set CHROME=...). Links to other .md files become links to the
# GitHub pages (a PDF has no relative links). Run again after changing the documents and commit the PDFs.
set -eu
cd "$(dirname "$0")/../docs"
CHROME=${CHROME:-$(command -v google-chrome || command -v chromium || command -v chromium-browser)}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/links.lua" <<'LUA'
-- relative links to Markdown files → their page on GitHub
local base = "https://github.com/ohlincha/libreramen/blob/main/docs/"
function Link(l)
  if not l.target:match("^%a+:") and l.target:match("%.md") then
    local t = l.target
    if t:match("^%.%./") then t = "https://github.com/ohlincha/libreramen/blob/main/" .. t:sub(4)
    else t = base .. t end
    l.target = t
  end
  return l
end
LUA
make_pdf() {   # markdown title pdf
    pandoc "$1" -f gfm -s --embed-resources --resource-path=. --lua-filter="$tmp/links.lua" \
        --metadata title="$2" -c manual.css -o "$tmp/x.html"
    "$CHROME" --headless --disable-gpu --no-pdf-header-footer --user-data-dir="$tmp/chrome" \
        --print-to-pdf="$PWD/$3" "file://$tmp/x.html" 2>/dev/null
    echo "$3"
}
make_pdf QUICKSTART.md "LibreRamen quick start" LibreRamen-QuickStart.pdf
make_pdf MANUAL.md "LibreRamen manual" LibreRamen-Manual.pdf
