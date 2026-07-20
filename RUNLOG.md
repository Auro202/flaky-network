# RUNLOG

Every row is a real harness run (`python3 run.py`), 30 s / 1500 frames.
Caps: deadline misses ≤ 1.00 %, bandwidth overhead ≤ 2.00×.
Profiles: **A** = 2 % loss, 10–40 ms jitter · **B** = 5 % loss, 20–80 ms jitter.

---

## 1. Baseline (handout, forward-once)

| Profile | Delay | Miss % | Overhead | Result |
|---|---|---|---|---|
| A | 40 ms | 22.67 % | 1.02× | INVALID |

Relay reported only **34 drops**, but 340 frames missed. So ~306 misses were
frames that *arrived late*, not lost. Two independent problems: jitter and loss.

## 2. Jitter buffer, paced playout

Buffer by seq; release each frame just before its deadline.

| Profile | Delay | Miss % | Overhead | Result |
|---|---|---|---|---|
| A | 100 ms | 2.27 % | 1.02× | INVALID |
| B | 100 ms | 5.40 % | 1.02× | INVALID |

Misses now equal relay drops exactly (34 / 81). Lateness solved; loss remains.

*(A first attempt released frames from a separate playout thread and scored
86 % — worse than baseline. A second, `select()`-driven single-threaded version
still drifted ~2 ms past deadlines. Timing margin, not logic, was the issue.)*

## 3. XOR parity FEC, K=4 — sender only, receiver ignores parity

| Profile | Delay | Miss % | Overhead | Result |
|---|---|---|---|---|
| A | 100 ms | 2.40 % | 1.29× | INVALID |
| B | 100 ms | 5.67 % | 1.29× | INVALID |

Sanity check: 1875 packets = 1500 data + 375 parity, overhead matches `1 + 1/K`.

## 4. Receiver reconstruction enabled

| K | Profile | Delay | Miss % | Overhead | Result |
|---|---|---|---|---|---|
| 4 | A | 100 ms | 1.20 % | 1.29× | INVALID |
| 4 | B | 100 ms | 3.93 % | 1.29× | INVALID |
| 2 | A | 100 ms | **0.20 %** | 1.55× | **VALID** |
| 2 | B | 100 ms | 1.67 % | 1.55× | INVALID |

K=4 recovered only ~half the drops — far worse than the IID-loss math predicts.
Cause is **timing, not coverage**: a group's parity cannot be computed until its
*last* frame arrives, so it lags the group's *earliest* frame by (K−1)×20 ms.
Rescuing that frame needs `(K−1)·20 + net_delay < delay_ms`; at K=4 on B that
holds only ~⅓ of the time. K=2 cuts the lag 60 ms → 20 ms. First VALID run.

## 5. SEND_EARLY sweep (paced receiver, K=2, profile B)

| SEND_EARLY | 90 ms | 100 ms | 110 ms |
|---|---|---|---|
| 3 ms | 23.33 % | 27.40 % | 21.60 % |
| 10 ms | 5.00 % | 1.80 % | 0.93 % |

Counter-intuitive: a *smaller* margin was catastrophically worse. Pacing was
losing races against OS scheduler wakeup jitter.

## 6. Forward-on-arrival (pacing removed)

The player records each frame's **first arrival** against a deadline — a latest
bound, not a schedule. Early delivery is never penalised, and the
receiver→player leg bypasses the relay, so it is free. Pacing was pure downside.

| Profile | 60 ms | 80 ms | 100 ms |
|---|---|---|---|
| A | **0.67 % VALID** | 0.20 % VALID | 0.20 % VALID |
| B | 35.60 % | 6.80 % | 1.00 % VALID |

A's floor drops 100 → 60 ms. B is valid but sits *exactly* on the 1.00 % cap.

## 7. Two-layer FEC (final)

Layer 0 (K=2, offset 0) + Layer 1 (K=3, offset 1): every frame sits in two
different groups, so it survives unless both are unrecoverable.

| Profile | 80 ms | 100 ms | Overhead |
|---|---|---|---|
| A | 0.00 % VALID | **0.00 % VALID** | 1.91× |
| B | 3.07 % INVALID | **0.33 % / 0.27 % VALID** | 1.91× |

Raising delay past ~100 ms cannot help B: its max jitter is 80 ms, so every
non-lost frame already arrives in time and the residual is pure unrecoverable
loss. That is why the fix had to be stronger FEC, not a larger buffer.

---

## Final configuration — submitted at `delay_ms = 100`

| Profile | Frames | Misses | Overhead | Result |
|---|---|---|---|---|
| A | 1500 | 0 (0.00 %) | 1.91× | **VALID** |
| B | 1500 | 4 (0.27 %) | 1.91× | **VALID** |

Chosen over A's 60 ms floor because a single `delay_ms` must hold on unseen
grading profiles; 100 ms clears B's cap with ~3× margin at 1.91× of a 2.00× cap.
