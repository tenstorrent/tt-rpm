# Branch-predictor algorithms

The algorithms here are plain C++: no Sparta, no clock. The host unit in
`src/core/Frontend/BranchPredictor/` calls them and owns all timing (redirect
latency, bubbles, branches per fetch). Parameters are documented in
[CONFIG_README](../src/core/CONFIG_README.md#branch-predictor).

## Adding a predictor

`gshare/src/Gshare.cpp` is a complete example in about 60 lines.

1. **Implement the interface** for the structure, from `common/`:

   | Structure | Interface | Selected by | Knobs | Generic keys the host seeds |
   |---|---|---|---|---|
   | Direction | `DirectionPredictorInterface` | `predictor_type` | `direction_args` | none |
   | BTB | `BtbInterface` | `btb_type` | `btb_args` | `entries`, `ways`, `tag_bits`, `replacement`, `rng_seed` |
   | RAS | `RasInterface` | `ras_type` | `ras_args` | `entries` |
   | Indirect target | `IndirectPredictorInterface` | `indirect_type` | `indirect_args` | none |

   No indirect-target algorithm ships: `indirect_type: none` uses the BTB's last
   target, and the interface is there for adding one.

   The generic keys carry the host parameters (`btb_entries`, `btb_ways`, ...),
   so every algorithm of that structure sees the same sizes. A user cannot
   repeat them in `*_args`; an algorithm need not read them.

2. **Take a `const PredictorArgs&`** in the constructor. `a.get<T>(key, default)`
   reads a knob (an unparsable value fails) and marks the key as known;
   `a.fail(key, why)` throws an error naming the parameter that set it. After
   construction the host rejects any `*_args` key the algorithm did not read.

3. **Register it**: `REGISTER_PREDICTOR(DirectionPredictorInterface, "gshare", Gshare)`.
   The key is the `*_type` value.

4. **Add the `.cpp` to `predictors.cmake`.** The core and cluster builds both
   include it.

5. **Document it** in CONFIG_README: the `*_type` choice, and a row for each
   `*_args` key under the algorithm.

6. **Counters (optional).** Override `stats()` to return `{name, desc, &counter}`
   for `uint64_t` members; the host reports them as
   `branch_predictor.<dir|btb|ras|ind>_<name>`.

## When the host calls what

For every fetched branch, in fetch order, wrong path included:

1. direction `predict()` (conditional branches), then `snapshot()`;
2. BTB `lookup()`; indirect `predict()` on a BTB hit typed as an indirect jump
   or call; RAS `predict()`, then `pop()`/`push()` as the branch type says, then
   RAS `snapshot()`;
3. indirect `snapshot()` and `speculativeUpdate()`, then direction
   `speculativeUpdate()`. Their `target` is what Decode sees: the static target
   of a direct branch, the predicted one of an indirect branch.

At retire: direction `update()` for conditional branches, BTB `allocate()` for
taken branches, and indirect `update()` for the indirect jumps and calls it
predicted. Nothing trains earlier.

**Checkpoints.** A history structure (direction, indirect) snapshots before its
speculative update; on a redirect the host restores it and re-applies the branch
with its resolved outcome and target. A target structure (RAS) snapshots after
its push or pop; a redirect restores the branch's own checkpoint, and a trap
the youngest older branch's. A checkpoint holds only speculative state:
restoring one must never undo commit-time training. The BTB trains only at
retire and has no checkpoint.

## Limitations

- The BTB is looked up at each branch's PC, which the trace identifies; it is
  not a fetch-block BTB that has to find the branches in a block.
- Branches in one fetch block are predicted one at a time, each seeing the
  older ones' speculative updates.
- Predictors start cold; there is no warm-up from a snapshot.
