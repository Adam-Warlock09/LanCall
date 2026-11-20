# results/

Numbers we'll plot or quote in the report. Unlike `logs/` and `experiments/` (both ignored by
git), everything here is committed, so it survives a `rm -rf` and both of us have it.

## Files

| File | What | Columns |
|---|---|---|
| `summary.csv` | Headline numbers from every test session so far, one number per row | phase, test, date, setup, side, metric, value, unit, note |
| `e4_cap_levels_run1.csv` | E4 first run: when the adaptive sender changed level. Cap on at ~15 s, off at ~40 s. Never climbed back because of bug #6, so re-run | t_s, level |
| `e2_loss_<laptop>.csv` | E2 raw table (copy it in, see below) | loss_pct, strips, ... as the script writes it |
| `e3_jitter_<laptop>.csv` | E3 raw table | jitter_ms, buffer_ms, audio_e2e_p50_ms, audio_e2e_p95_ms, late_drops, concealed |
| `e4_<mode>_persecond.csv` | E4 per-second receiver stats, adaptive and fixed | as `log_summary --per-second` writes it |
| `e8_repair_<laptop>.csv` | E8 (phase 5) raw table: loss vs none / NACK / FEC / both | loss_set_pct, mode, shown_fps, complete_frames_pct, loss_after_repair_pct, nack_sent, resent, fec_recovered, upload_kbps |

`summary.csv` is in long format (one value per row) so a plot script can filter it with one
line, e.g. in pandas `df[(df.test == "A1") & (df.metric == "audio_e2e_p50")]`. Bug-affected
numbers stay in, with the bug in `note`, so the report can show before/after.

## Adding an experiment run

The experiment scripts write to `experiments/<name>_<time>/`. Copy the tables over with a name
that says which laptop and, if it's a re-run, which run:

```bash
cp experiments/e2_loss_*/results.csv          results/e2_loss_aric.csv
cp experiments/e3_jitter_*/results.csv        results/e3_jitter_aric.csv
cp experiments/e4_cap_*/adaptive_levels.csv   results/e4_cap_levels_run2.csv
cp experiments/e4_cap_*/adaptive_persecond.csv results/e4_adaptive_persecond_run2.csv
cp experiments/e4_cap_*/fixed_persecond.csv   results/e4_fixed_persecond_run2.csv
cp experiments/e8_repair_*/results.csv        results/e8_repair_aric.csv
```

(if there's more than one run folder, use the exact folder name instead of `*`), then add a
line or two to `summary.csv` for the headline numbers and commit both.
