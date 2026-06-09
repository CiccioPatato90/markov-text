-- Stage the document as positioned, sentence-segmented, lowercased tokens.
-- Sentinels <s>/</s> wrap each sentence so generation can begin and end.
-- Run once per :train; the per-order n-gram merge (train_order.sql) reads doc_tokens.
CREATE OR REPLACE TEMP TABLE doc_tokens AS
WITH raw AS (
    SELECT content FROM read_text('{{PATH}}')
),
sentences AS (
    SELECT s_id,
           list_concat(['<s>'],
                       regexp_extract_all(replace(lower(sent), '’', ''''), '[\p{L}][\p{L}'']*'),
                       ['</s>']) AS toks
    FROM (
        SELECT generate_subscripts(parts, 1) AS s_id, unnest(parts) AS sent
        FROM (SELECT string_split_regex(content, '[.!?]+') AS parts FROM raw)
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
