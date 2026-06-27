-- Stage the document as positioned, sentence-segmented, lowercased tokens.
-- Sentinels <s>/</s> wrap each sentence so generation can begin and end.
-- Run once per :train; the per-order n-gram merge (train_order.sql) reads doc_tokens.
--
-- {{READER}} is replaced by the C driver with a source-specific reader that
-- yields one column named `content`:
--   .txt              one row with the whole file        (read_text)
--   .parquet / .csv   one row per record, chosen column  (read_parquet/read_csv)
-- Everything below is source-agnostic: each content row is sentence-split
-- independently, and s_id is globally unique across all rows (the n-gram
-- window in train_order.sql partitions by s_id, so ids must never collide
-- between two content rows).
CREATE OR REPLACE TEMP TABLE doc_tokens AS
WITH raw AS (
    {{READER}}
),
cleaned AS (
    -- ------------------------------------------------------------------
    -- <unk> filter.
    --
    -- Closed-vocabulary corpora (wikitext-103 and similar benchmark sets)
    -- replace every out-of-vocabulary word with the literal marker "<unk>".
    -- Our tokenizer keeps only letter runs ([\p{L}][\p{L}']*), so the angle
    -- brackets would be stripped and the marker would survive as the bare
    -- token "unk". Measured on wikitext-103: ~225K occurrences in one shard
    -- — more frequent than most real words — and generation would emit
    -- nonsense like "the unk of unk".
    --
    -- Replacing the marker with a space BEFORE tokenization removes it
    -- entirely. Sentence structure is unaffected: the marker only ever
    -- stands in word position, never carries punctuation. For corpora
    -- without this convention the replace is a no-op — the literal string
    -- "<unk>" effectively never occurs in natural text.
    -- ------------------------------------------------------------------
    SELECT replace(content, '<unk>', ' ') AS content FROM raw
),
sentences AS (
    SELECT row_number() OVER () AS s_id,   -- global: unique across content rows
           list_concat(['<s>'],
                       regexp_extract_all(replace(lower(sent), '’', ''''), '[\p{L}][\p{L}'']*'),
                       ['</s>']) AS toks
    FROM (
        SELECT unnest(string_split_regex(content, '[.!?]+')) AS sent
        FROM cleaned
    )
    WHERE len(regexp_extract_all(replace(lower(sent), '’', ''''), '[\p{L}][\p{L}'']*')) > 0
)
SELECT s_id,
       p.pos AS pos,
       toks[p.pos] AS token
FROM sentences
CROSS JOIN UNNEST(generate_series(1, len(toks))) AS p(pos);

-- Upsert global vocabulary frequencies.
INSERT INTO vocabulary (token, freq)
SELECT token, COUNT(*) FROM doc_tokens GROUP BY token
ON CONFLICT (token) DO UPDATE SET freq = vocabulary.freq + excluded.freq;

-- Bookkeeping: one documents row per :train invocation.
INSERT INTO documents (name, token_count)
SELECT '{{PATH}}', (SELECT COUNT(*) FROM doc_tokens);
