For what this is — a model whose every sampling step is a SQL round-trip — they're good, and more importantly they're explainable,
  which is what your report needs. Let me break the number down.

  Where the 0.75 ms/token goes. Each token costs one prepared SELECT (the weighted sample), often one or two more SELECTs for backoff
  misses, and one auto-committed INSERT into generation_steps. Almost none of that is data work — your transitions table is 3.4K rows,
  which DuckDB chews through instantly. The cost is fixed per-statement overhead: query setup, vectorized execution machinery designed
  for millions of rows being spun up for one, and a WAL commit per logged step. That's why the rate is so stable across your three runs
  (947 → 1351 → 1340 tok/s): it doesn't depend on output length, and it would barely change if your corpus were 100× bigger. That's
  the headline for the report — an OLAP engine has high fixed cost and near-zero marginal cost per query, which is exactly the inverse
  of what a per-token loop wants. Your own numbers demonstrate the README's central argument: training, the batched set-based path,
  ingests 1,262 tokens in ~40 ms (~30K tok/s); generation, the row-at-a-time path, runs at ~1.3K tok/s — a 20× gap between DuckDB's
  happy path and its unhappy one, measured on your machine.

  Calibration against the alternatives. An in-RAM hash-map Markov chain would generate millions of tokens per second — you're paying
  roughly 1000× for keeping the model in the database, and what you bought is persistence, incremental training, and the analytics
  layer. That's the honest trade and it's a deliberate one. (For fun: you're also ~10× faster than an LLM generates text, which is a
  cute line for the oral exam, though it's apples and oranges.)

  If you ever wanted it faster, in order of payoff: wrap each :gen run in one BEGIN/COMMIT so the step inserts share a single WAL
  commit instead of one each (likely the biggest win, ~3 lines); buffer step logs and insert them in one batch at the end of the run
  (moves logging onto the set-based happy path); and merge the backoff into a single query (WHERE ord <= $1 ... ORDER BY ord DESC with
  the reservoir key, taking the best row of the highest order present) so each token costs exactly one SELECT. I wouldn't spend line
  budget on any of these — 22 seconds for a 30K-token ceiling is fine — but measuring the transaction-wrapping one would make a nice
  table in the report.

  One small thing visible in run 15: you asked for 32,704 and got 29,747 because the history buffer also holds the <s>/</s> sentinels
  (~3K sentences ≈ 6K extra slots). The "stopped at history limit" reason is telling the truth, but if the off-by-sentinels bothers
  you, the cap could count tokens instead of history slots.
