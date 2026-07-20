# NOTES

**Graded playout delay: `delay_ms = 100`** (profile A 0.00 % misses, profile B
0.27 %, bandwidth overhead 1.91× — both VALID).

The baseline's 340 misses against only 34 relay drops showed the problem was two
separate things, late frames and lost frames, so I fixed them with two separate
mechanisms. For loss I chose forward error correction over retransmission,
because a resend costs two crossings of the hostile link and would push the
playout delay — the thing actually being scored — far above what proactive
redundancy costs in bytes. The sender XORs groups of frames into parity packets;
the receiver rebuilds any single missing frame in a group as parity XOR the
survivors. The parameter that matters most is the group size K, because a
group's parity cannot be computed until its last frame arrives and therefore
lags the group's earliest frame by (K−1)×20 ms, so large K produces parity that
is useless by the time it lands — measured as K=4 recovering only half the drops
while K=2 recovered nearly all of them. The second significant finding was that
pacing output to the deadline was actively harmful: the harness scores each
frame's *first* arrival against a latest-bound deadline and the receiver→player
leg does not cross the relay, so forwarding every frame the instant it exists is
free and strictly better, which alone took profile A's floor from 100 ms to
60 ms. I settled on two overlapping parity layers (K=2 at offset 0, K=3 at
offset 1) so each frame has two independent recovery chances, since a single
layer left profile B sitting exactly on the 1 % cap and no amount of extra delay
could help — B's jitter maxes at 80 ms, so at 100 ms every non-lost frame is
already in time and the residual is pure unrecoverable loss. I submitted 100 ms
rather than A's 60 ms floor because one delay value must hold on unseen grading
profiles, and 100 ms clears B's miss cap with roughly 3× margin while spending
1.91× of the 2.00× byte budget. The main risk I accepted is burst loss: both
layers use consecutive grouping, so a burst long enough to take two members of
the same group defeats recovery, and the interleaving support I built (STRIDE)
was deliberately left at 1 because spreading a group over more frames widens its
span and reintroduces exactly the parity-lag problem that K=4 demonstrated.
