

# bogosort crowd-compute — client API

Build your own client to contribute shuffles to the 24/7 bogosort stream.
This is the complete, current protocol. If your client follows it exactly you
get credited; if it deviates, your reports bounce. There are **no batches** —
that model is gone.

---

## Endpoint

```
wss://bogo.swapjs.dev/ws
```

WebSocket, JSON, one text frame per message. (`ws://<ip>/ws` also works for local
testing, but the public host is `wss`.)

---

## The model, in one paragraph

You open **one socket per machine**. You say `hello`. The server hands you a
**lease**: a `seed` and a `count`. Every integer index `i` in `[0, count)`
defines exactly one independent 25-element shuffle, derived purely from
`(seed, i)` — there is no continuous RNG stream, each shuffle stands alone. Your
job is to compute as many of those shuffles as you can and, ~once a second, tell
the server (a) how many you've done so far (cumulative) and (b) the **best** one
you've found — the shuffle with the most elements already sitting in their final
sorted position — together with the **exact index** it came from. The server
replays that one index in O(1) to confirm you're not lying.

---

## Message flow

```
client → hello
server → welcome        (your account state)
server → job            (your lease: seed + count)
client → result         (every ~1s: cumulative progress + best)
server → credited       (per accepted report)
   … repeat result/credited …
server → job            (a fresh lease, once you exhaust count)
```

---

## Messages

### client → server

#### `hello`
```json
{ "type": "hello", "v": 5, "uuid": "1a2b...", "nickname": "mybot", "code": "" }
```
| field | notes |
|---|---|
| `v` | protocol version — send **5**. Lower still credits while reports are clean, but you get a `client_outdated` nudge the instant a report is malformed. |
| `uuid` | your stable id, 16–64 hex chars (dashes ok). Generate **once**, persist it. |
| `nickname` | 2–8 chars, validated (no slurs / reserved names). First hello with a fresh uuid registers it; reconnects must match. |
| `code` | recovery code. On your very first connect send `""` — the server issues one in `welcome`. **Store it** and send it on every future connect to prove you own the uuid. |

#### `result` — once per ~second
```json
{ "type": "result",
  "seed": "1780586389331",
  "total_done": 1234567,
  "best_correct": 12,
  "best_arr": [1,2,17,4,5,...],
  "best_index": 9876 }
```
| field | notes |
|---|---|
| `seed` | the current lease seed — echo it back exactly. |
| `total_done` | **cumulative** shuffles computed on this lease. Integer, strictly increasing across reports, ≤ `count`. |
| `best_correct` | highest fixed-point count you've found (positions `k` where value == `k+1`), 0–25. |
| `best_arr` | the actual 25-element board that achieved `best_correct` — a permutation of `1..25` containing exactly `best_correct` fixed points. |
| `best_index` | the global index `i` whose shuffle produced `best_arr`. **Must be the real index** — the server replays `shuffle(seed, best_index)` and requires it to equal `best_arr`. |

#### `stop`
```json
{ "type": "stop" }
```
Drops your session cleanly.

### server → client

#### `welcome`
```json
{ "type": "welcome", "uuid": "...", "nickname": "...",
  "lifetime_shuffles": 0, "all_time_best": 0, "badges": [], "batch_size": 5000000 }
```
Account state. **`batch_size` is vestigial — ignore it.** If this is a new uuid,
the response carries your issued recovery `code` — persist it.

#### `job` — your lease
```json
{ "type": "job", "seed": "1780586389331", "count": 100000000000000 }
```
| field | notes |
|---|---|
| `seed` | a 64-bit integer **as a string** (it can exceed 2^53). Parse as u64 / BigInt. |
| `count` | size of your index window (currently `1e14`). You work indices `[0, count)`. |

#### `credited`
```json
{ "type": "credited", "seed": "...", "credit": 60000000,
  "lifetime_shuffles": 123, "rate": 60000000,
  "batch_best": 12, "my_tick_best": 12, "my_session_best": 14,
  "all_time_best": 14, "new_badges": [] }
```
`credit` = how many shuffles were actually banked from this report. **It may be
less than your reported delta** — see the clamp.

#### `rejected`
```json
{ "type": "rejected", "reason": "bad_best_index", "seed": "..." }
```
No credit for that report. Your **lease stays valid** — fix the report and keep
going. Reasons table below.

#### others
`client_outdated` (update your client), `banned` (`{reason, expires_at}`),
`contributions_closed` (operator paused new work). Handle by stopping/updating.

---

## The kernel — must be byte-identical

The server verifies you by recomputing one shuffle and comparing. **One bit off
and every report bounces.** Fastest path: just run the published wasm,
`https://bogo.swapjs.dev/bogo.wasm`. If you're porting (e.g. to a GPU), match the
following exactly.

Each shuffle at global index `i` is fully independent:

```
shuffle_seed(seed64, i) = (seed64 + i * 0x9e3779b97f4a7c15) mod 2^64
```

Derive a xoshiro128++ state via two SplitMix64 steps:

```
splitmix64(state):                  # state is u64, carried between calls
  state = (state + 0x9e3779b97f4a7c15) mod 2^64
  z = state
  z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) mod 2^64
  z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) mod 2^64
  return z ^ (z >> 31)

xseed(seed_i):                      # -> 4 x u32
  state = seed_i
  a = splitmix64(state); b = splitmix64(state)    # state carries across the two
  s = [ a & 0xffffffff, (a >> 32) & 0xffffffff,
        b & 0xffffffff, (b >> 32) & 0xffffffff ]
  if s == [0,0,0,0]: s[0] = 1
  return s
```

xoshiro128++ (all ops u32):

```
rotl32(x, k) = ((x << k) | (x >> (32 - k))) & 0xffffffff

next(s):
  result = (rotl32((s[0] + s[3]) & 0xffffffff, 7) + s[0]) & 0xffffffff
  t = (s[1] << 9) & 0xffffffff
  s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]
  s[2] ^= t; s[3] = rotl32(s[3], 11)
  return result
```

Unbiased index in `[0, max)` by rejection sampling:

```
xint(s, max):
  thr = (2^32) mod max
  repeat: x = next(s)  until x >= thr
  return x mod max
```

The shuffle — Fisher–Yates over `[1..25]`:

```
one_shuffle(seed_i):
  s = xseed(seed_i)
  a = [1, 2, 3, ..., 25]
  for i from 24 down to 1:
    j = xint(s, i + 1)
    swap a[i], a[j]
  correct = count of 0-based positions k where a[k] == k + 1
  return (correct, a)
```

So the board + score at index `i` is `one_shuffle(shuffle_seed(seed, i))`. To find
your "best" over a window, run this for each `i` and keep the max `correct` with
its board `a` and its index `i`.

**Reference implementations to diff against:** `coordinator/xoshiro.js` (JS),
`bogo-wasm/src/lib.rs` (Rust), `coordinator/static/bogo.wasm` (compiled).

---

## What makes a report valid (server checks, in order)

1. **`too_fast`** — arrived < 5 ms after your previous report.
2. **`bad_total`** — `total_done` not an integer, not strictly greater than what was last credited, or > `count`.
3. **`bad_best_correct`** — not 0–25. **`bad_best_arr`** — not a permutation of `1..25`.
4. **`best_arr_inconsistent`** — `best_arr` doesn't contain exactly `best_correct` fixed points.
5. **`bad_best_index`** — `best_index` missing / not an integer in `[0, count)`.
6. **replay** — server computes `one_shuffle(shuffle_seed(seed, best_index))` and requires it to equal `(best_correct, best_arr)`. Mismatch → **`verify_mismatch`** / **`best_arr_mismatch`**.
7. credits the delta (subject to the clamp).

> **The #1 mistake custom clients make is steps 5–6:** sending a `best_arr`
> without the exact `best_index` it came from. You must **track which index
> produced your best board** and send that integer. `0`, `null`, or a stale index
> → `bad_best_index` / `verify_mismatch` every single time.

---

## The volume clamp — report honestly

Your **lifetime** credited total is capped by the best board you've actually found:

```
cap    = factorial(best_correct) / 0.37 * 64
credit = min(reported_delta, cap - current_total)
```

Rationale: finding a board with `b` fixed points takes ~`b!` shuffles of real
search, so your provable volume is tied to your provable best. Inflating
`total_done` past what your best supports is clamped to zero extra — it doesn't
pay. Report your real numbers and you get full credit.

---

## Rejection reasons

| reason | meaning / fix |
|---|---|
| `too_fast` | reported < 5 ms after the last one. Target ~1/sec. |
| `bad_total` | `total_done` must be a strictly-increasing integer ≤ `count`. |
| `bad_best_correct` | must be 0–25. |
| `bad_best_arr` | must be a permutation of `1..25`. |
| `best_arr_inconsistent` | `best_arr` must contain exactly `best_correct` fixed points. |
| `bad_best_index` | `best_index` must be an integer in `[0, count)` — the real index of `best_arr`. |
| `verify_mismatch` | replay of `seed@best_index` gave a different score. Your index/board don't match. |
| `best_arr_mismatch` | replay gave a different board. |
| `anomaly` | your best-score running average sat below the honest floor too long. Report your real bests. |
| `unknown_seed` | reported against a stale seed. Always echo the latest `job` seed. |

A rejection earns no credit but does **not** drop your lease — fix and continue.

---

## Cadence + etiquette

- **One socket per machine.** Use as many threads/GPUs internally as you want;
  aggregate and report through the single socket.
- **Report ~once per second** with cumulative `total_done`. Faster gains nothing
  (and < 5 ms bounces).
- Slice the index space however you like — compute a chunk (e.g. 8M indices),
  check the clock, report. The server doesn't care how you chunk it.

---

## Minimal reference client (Node, using the JS kernel)

```js
import WebSocket from 'ws';
import { runRange } from './coordinator/xoshiro.js'; // runRange(seed64,lo,hi) -> {best_correct,best_arr,best_index}

const UUID = '<persist me>', NICK = 'mybot';
let CODE = '<persist me, or "">';

const ws = new WebSocket('wss://bogo.swapjs.dev/ws');
let seed = null, count = 0n, cursor = 0n, total = 0;
let best = { best_correct: -1, best_arr: null, best_index: 0 };

ws.on('open', () => ws.send(JSON.stringify({ type: 'hello', v: 5, uuid: UUID, nickname: NICK, code: CODE })));

ws.on('message', (d) => {
  const m = JSON.parse(d);
  if (m.type === 'welcome') { if (m.code) CODE = m.code; /* persist CODE */ }
  else if (m.type === 'job') {
    seed = BigInt(m.seed); count = BigInt(m.count);
    cursor = 0n; total = 0; best = { best_correct: -1, best_arr: null, best_index: 0 };
  }
  else if (m.type === 'rejected') console.warn('rejected:', m.reason);
});

// Illustrative loop. A real client grinds the kernel flat-out, not on a timer.
setInterval(() => {
  if (seed === null) return;
  const lo = cursor;
  const hi = (cursor + 8_000_000n > count) ? count : cursor + 8_000_000n;
  const r = runRange(seed, lo, hi);          // best over [lo, hi)
  cursor = hi; total += Number(hi - lo);
  if (r.best_correct > best.best_correct) best = r;   // r.best_index is a GLOBAL index
  if (ws.readyState === 1 && best.best_arr) {
    ws.send(JSON.stringify({
      type: 'result', seed: seed.toString(), total_done: total,
      best_correct: best.best_correct, best_arr: best.best_arr, best_index: best.best_index,
    }));
  }
}, 1000);
```

That's the whole protocol: one lease, one index space, your honest best with its
real index, once a second.
