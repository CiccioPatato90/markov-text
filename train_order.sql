-- Build order-{{K}} (state -> next) n-grams within sentence boundaries
-- and merge them into the model. Run once per order 1..k after train_stage.sql.
CREATE OR REPLACE TEMP TABLE ngrams AS
SELECT
    string_agg(token, ' ') OVER w AS state_key,    -- k tokens: CURRENT .. (k-1) FOLLOWING
    lead(token, {{K}}) OVER (PARTITION BY s_id ORDER BY pos) AS next_token,
    COUNT(*) OVER w AS span                        -- == k only when the full window is present
FROM doc_tokens
WINDOW w AS (PARTITION BY s_id ORDER BY pos
             ROWS BETWEEN CURRENT ROW AND {{KM1}} FOLLOWING);

-- Merge into the model (one set-based upsert -- DuckDB's happy path).
INSERT INTO transitions (ord, state_key, next_id, count)
SELECT {{K}}, g.state_key, v.id, COUNT(*)
FROM ngrams g
JOIN vocabulary v ON v.token = g.next_token
WHERE g.next_token IS NOT NULL AND g.span = {{K}}
GROUP BY g.state_key, v.id
ON CONFLICT (ord, state_key, next_id)
  DO UPDATE SET count = transitions.count + excluded.count;

-- Bookkeeping: one training_runs row per (document, order).
INSERT INTO training_runs (document_id, ord, ngrams_added)
SELECT currval('seq_doc'), {{K}},
       (SELECT COUNT(*) FROM ngrams WHERE next_token IS NOT NULL AND span = {{K}});
