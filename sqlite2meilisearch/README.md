# sqlite2meilisearch

Fully synchronize a Meilisearch index from SQLite: SQL selects rows, a Rhai script maps each row
to a document, and the index is reconciled by primary key — stale documents are deleted.

**Destructive, not incremental.** The output must be the complete document set; errors are not
rolled back. Use an isolated candidate index and publish only after a successful run.

## Usage

```bash
sqlite3 /tmp/demo.sqlite < examples/seed.sql

# Preview the first document (no Meilisearch needed)
sqlite2meilisearch \
    --sqlite /tmp/demo.sqlite \
    --query examples/products.sql \
    --transform examples/products.rhai \
    --preview

export MEILI_URL=http://127.0.0.1:7700
export MEILI_KEY=may-be-empty-key

# Full sync
sqlite2meilisearch \
    --sqlite /tmp/demo.sqlite \
    --query examples/products.sql \
    --transform examples/products.rhai \
    --settings examples/settings.json \
    --index products_candidate
```

The Rhai script passed with `--transform` must define `fn transform(row)`. It receives a map of
the row and returns a map to emit it as a document, or `()` to skip the row; the returned map must
contain the primary key field (`--primary-key`, default `id`). Omit `--transform` to pass rows
through unchanged.

