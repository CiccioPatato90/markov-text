#!/bin/sh
# Package a Linux release: markov-v<VERSION>/ next to this script, then zip it.
# Version comes from MARKOV_VERSION in markov_utils.h.
#
#   ./deploy.sh             binary + libduckdb.so + install.sh + sample corpus
#   ./deploy.sh --with-db   also ship data/db/markov.db as a pre-trained seed
set -eu
cd "$(dirname "$0")"

VERSION=$(sed -n 's/^#define MARKOV_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' markov_utils.h)
[ -n "$VERSION" ] || { echo "MARKOV_VERSION not found in markov_utils.h" >&2; exit 1; }
command -v zip >/dev/null || { echo "zip not installed" >&2; exit 1; }

PKG="markov-v$VERSION"

make clean >/dev/null
make

rm -rf "$PKG" "$PKG.zip"
# bin/ + libs/linux/ mirrors the repo layout so the binary's relative
# rpath ($ORIGIN/../libs/linux) resolves inside the package too
mkdir -p "$PKG/bin" "$PKG/libs/linux"
cp build/markov "$PKG/bin/"
cp libs/linux/libduckdb.so "$PKG/libs/linux/"
cp data/short.txt "$PKG/sample.txt"

if [ "${1:-}" = "--with-db" ]; then
    [ -f data/db/markov.db ] || { echo "--with-db: data/db/markov.db not found" >&2; exit 1; }
    if [ -s data/db/markov.db.wal ]; then
        echo "--with-db: data/db/markov.db.wal is not empty — quit markov first" >&2
        echo "so pending writes are checkpointed into the db file" >&2
        exit 1
    fi
    cp data/db/markov.db "$PKG/markov.db.seed"
fi

cat > "$PKG/install.sh" <<'EOF'
#!/bin/sh
# One-time setup — run from anywhere, works on the package directory.
set -eu
cd "$(dirname "$0")"
mkdir -p data/db output
chmod +x bin/markov
if [ -f markov.db.seed ] && [ ! -f data/db/markov.db ]; then
    cp markov.db.seed data/db/markov.db
    echo "seeded pre-trained model into data/db/markov.db"
fi
echo "installed — run ./bin/markov from this directory (:help inside)"
EOF
chmod +x "$PKG/install.sh"

cat > "$PKG/README.txt" <<EOF
markov v$VERSION — DuckDB-backed multi-order Markov text engine

  ./install.sh           one-time setup (creates data/db)
  ./bin/markov           start the REPL; the model persists in data/db/markov.db

inside the REPL:
  :train sample.txt 3    train orders 1..3 on the bundled sample corpus
  :gen 3 100             generate 100 tokens with order-3 backoff
  :stats size            model size per order
  :help                  all commands

train on your own corpus: drop any .txt next to this file and
:train yourfile.txt <order> — paths are relative to this directory.
EOF

zip -qr "$PKG.zip" "$PKG"
echo "packaged: $PKG.zip ($(du -h "$PKG.zip" | cut -f1))"
