# LanCall Phase 10 test plan: essentials (Phases 4–9, final build)

Goal: enough testing to claim every feature and fill the report's key metrics.
- **Main plan:** 3 Linux laptops, **≈1 h 45 min**, plus a 10 min wrap-up afterwards.
- **Windows:** in its own section at the end, run once Phase 8 can run lancall.

## Hardware and roles

| Id | OS | Floor | Fixed jobs |
|---|---|---|---|
| L1 | Linux | A | loopback experiments, `lancall_directory` all session, group host |
| L2 | Linux | A | floor-A partner |
| L3 | Linux | B | floor-B partner, relay host, `lancall_bridge` |

- There's one wall port per room.
- Log names: `--log logs/<test>_<laptop>.csv`.
- Every lancall gets `--directory <L1 ip>`.
- Headphones in every call. netem off after every use.

## Build status (main at `5b6a718`, checked 20 Nov)

| Phase | On main | Still missing |
|---|---|---|
| 8 Windows | platform layer and socket class on Winsock; CI on Ubuntu and Windows (libraries and their tests only) | signaling, discovery, camera, audio, and the `lancall` / `lancall_relay` / `lancall_directory` programs on Windows. Every Windows test is blocked until these land: see the Windows section at the end |
| 9 UI | app core shared by `lancall` and `lancall_gui`. GUI: start screen, Peers panel (Call / Add / Watch), Call panel (incoming popup, members with safety codes, mesh / relay / multicast, Hang up), video grid, and a messages panel with a command box | no separate chat, stats or settings panels; chat goes through the command box. Needs `libglfw3-dev`. Logs the same rows as lancall |
| Bugs | — | #11 (frozen tile on an immediate rejoin) and the `av_sync_ms` outlier: not fixed, not in the NOTES.md issue table |

The terminal `lancall`'s flags, commands, `list` output and log events are unchanged, so all
the scripts work as before.

## Already done: not repeated

The tests below ran on the pre-Phase-5 build and are the Phase 4 results:
- 1a self-call: audio p50 84.0 ms;
- 1b group signaling;
- 2a 1:1 call: audio p50 88.2 ms, 15 fps;
- 2b 5% loss: #7 fixed, no stall;
- 2c bandwidth cap;
- 2d relay pair;
- 4a 3-person cross-floor mesh: audio p50 80.9 ms, 0% loss;
- 4b rejoin: works after 10 s, **frozen tile on an immediate rejoin (#11, open)**;
- E6: the cross-floor penalty is about 0.

4c wasn't done; G2 covers it.

## Before the session (not counted)

| # | Task |
|---|---|
| P1 | Fix or document #11 and the `av_sync_ms` outlier from the partial 4c log. Add both to the NOTES.md issue table |
| P2 | Done: `--no-nack` in E2, plus the helper scripts `call_timings.sh`, `exp_e7_soak.sh` and `exp_d1_discovery.sh` |
| P3 | Aric, offline: `log_summary --per-second logs/2b_Aric.csv` (≈5% during the both-sides part closes #9). Check that the last `video_level` in `logs/2c_Aric.csv` is 0 |
| P4 | Tag `test-final`. **Every laptop:** `sudo apt install libglfw3-dev`, build, run `ctest` (save the output) and `devcheck`, set `sudo sysctl -w net.core.rmem_max=8388608`, and check that `build/gui/lancall_gui` exists (without GLFW it is skipped silently) |
| P5 | Start `lancall_gui` from a normal terminal (Ctrl+Alt+T), not VS Code's: the snap build of VS Code makes the GUI abort (NOTES.md) |

## Timeline (T = minutes)

| Slot | T | L1 (A) | L2 (A) | L3 (B) |
|---|---|---|---|---|
| S0 | 0–10 | start directory, post IPs | post IPs | post IPs |
| S1 | 10–35 | loopback: E8, E4, E3, E9, E1, D1 | 1:1 with L3: C1, C2, C5, C7, C8, C6 | 1:1 with L2 |
| S2 | 35–55 | 15 min soak with L2 | 15 min soak | E2 (spare, loopback) |
| S3 | 55–75 | group host: E5, G4, G7, G2 | member | member, then relay host |
| S4 | 75–95 | multicast + broadcast host, E10 presenter | viewer / idle | bridge, viewer |
| S5 | 95–105 | **UI** (`lancall_gui`) | **UI** | terminal member |
| after | +10 | wrap-up | wrap-up | wrap-up |

**If behind**, cut in this order: E3 → the soak (to 10 min) → E10 → G7.

---

## S0: start (10 min)
- L1: `./build/control/lancall_directory` in its own terminal, left running.
- Everyone: post `ip -4 addr | grep 10.8.` to the group chat. If ufw is on:
  `sudo ufw allow 5004:5016/udp && sudo ufw allow 5011/tcp`.

## S1 (25 min)

### L1: loopback experiments, strictly one at a time

| Step | Command | Time | Report numbers / pass |
|---|---|---|---|
| E8 | `LOSSES="5 10 20" DURATION=10 tools/exp_e8_repair.sh` | 3 min | complete frames and residual loss for none / NACK / FEC / both; upload cost |
| E4 | `tools/exp_e4_cap.sh` | 3 min | steps down < 5 s after the cap, climbs back to L0 after it (#6) |
| E3 | `JITTERS="0 20 40" BUFFERS="40 60 100" DURATION=15 tools/exp_e3_jitter.sh` | 3 min | smallest buffer with late drops ≈ 0 for each jitter |
| E9 | `./build/tools/crypto_bench \| tee logs/E9_L1.txt` | 1 min | µs per packet to seal/open, size overhead |
| E1 | `timeout -s INT 90 ./build/call/lancall --name T --peer 127.0.0.1 --log logs/E1_L1.csv` | 1.5 min | final latency and A/V sync (`log_summary`) |
| D1 | `tools/exp_d1_discovery.sh` | 1 min | discovery time, target < 5 s |

### L2 ↔ L3: cross-floor 1:1 on the final build
`./build/call/lancall --name <me> --directory <L1 ip> --log logs/<test>_<me>.csv`. L2 calls L3.

| Test | Steps | Time | Report numbers / pass |
|---|---|---|---|
| C1 clean | call, 2 min, `stats`, `members` | 3 min | "encrypted"; **same safety code on both**; audio/video p50, fps, rtt (`log_summary`); setup time (`tools/call_timings.sh`): accept → first frame < 2 s |
| C2 5% loss | `tools/netem.sh on 5` on both, 2 min, `stats` at 1 min, `off` | 3 min | measured loss ≈ 5%, residual after NACK ≈ 0, `nack N` > 0, no stall |
| C5 outage | clean call; L3 runs `tools/netem.sh on 100; sleep 1; tools/netem.sh off` twice, 20 s apart | 2 min | `call_timings.sh logs/C5_L2.csv`: recovery (gap − 1 s) < 2 s |
| C7 chat | `text L3 hi` outside a call; `text all …` in a call; to an offline name; a 501-byte message; emoji | 3 min | delivered, or a clear error |
| C8 old client refused | `nc <L2 ip> 5011`, then type `INVITE` | 1 min | answered `DECLINE reason=old-version` |
| C6 Ctrl+C | Ctrl+C on L3 mid-call | 1 min | L2's call ends cleanly; calling again works |

---

## S2 (20 min)

### L1 ↔ L2: E7 soak (15 min)
```bash
WARMUP_S=60 WINDOW_S=180 MIN_CALL_S=840 tools/exp_e7_soak.sh run L1    # L2 likewise
```
- L1 calls L2. Don't touch it for 15 min; then `hangup`, then `quit`.
- **Pass:** from the printed summary: memory growth < 10%, audio p50 moved < 5 ms, fps
  steady. The mean CPU is the encrypted call's CPU (E9).

### L3: E2 (spare)
`DURATION=10 tools/exp_e2_loss.sh`: whole frames vs 10 strips at 0–20% loss, final build,
NACK off.

---

## S3 (20 min): group calls

### E5, mesh vs relay (2 runs, ~5 min)
- Members run `EXTRA="--directory <L1 ip>" tools/exp_e5_group.sh run <mode> 3 <name>`.
- Host L1 runs `EXTRA=… RELAY=me tools/exp_e5_group.sh run <mode> 3 L1 L2 L3`.
- Runs: **mesh 3, relay 3**. The 2-person numbers come from C1.
- **Report:** upload per sender (mesh ≈ 2 copies, relay 1), latency in each mode, 15 fps.
  Collect into `results/e5_group.csv`.

### One call: L1 hosts L2 and L3 (~15 min)

| Test | Steps | Pass |
|---|---|---|
| G4 group chat + codes | L2 `text all hi`; L2 `text L3 psst`; `members` on all | delivered as addressed; safety codes match per pair |
| G7 dead member | pull L3's cable for 20 s, plug it back, L1 `add L3` | the others drop L3 after ~10 s; the call carries on; the re-add works |
| G2a relay in the call | L1 `relay me`, 1 min, `mesh` | media continues; `call_timings.sh` gaps < 2 s; `members` agrees |
| G2b relay on a non-member (replaces 4c) | L3 hangs up and runs `./build/net/lancall_relay`; L1 `relay <L3 ip>` with L2, 1 min, then `mesh` | media continues through L3; the relay prints 2 members; `av_sync_ms` min is sane (`log_summary`) |
| G6 rejoin (#11) | **only if P1 fixed it:** L3 `hangup`, L1 `add L3` at once | L3's tile is live on L2 |

---

## S4 (20 min): multicast, broadcast, E10

| Test | Steps | Pass |
|---|---|---|
| M1 multicast call + auto switch | L1 calls L2, `multicast`, 1 min, `stats`; then `add L3` | `multicast 239.255.77.x` with one upload copy; after the add, **switches to a relay on its own**, and L3 gets media (`call_timings.sh` for the gap) |
| B1 broadcast | L1 `broadcast demo`; L2 `watch L1` 1 min, `unwatch`; L1 tries `call L3` | L2 sees and hears it; the call is refused while broadcasting |
| B3 bridge | L1 (still broadcasting) `bridge <L3 ip>`; L3 runs the printed `lancall_bridge` line, then `watch L1-bridge` in its own lancall | video plays on floor B via the bridge |

### E10, reduced (3 runs × ~2 min)
The commands are at the top of `tools/exp_e10_multicast.sh`.

| Run | L1 | L2 | L3 |
|---|---|---|---|
| unicast 2 | presenter | viewer | viewer |
| multicast 1 | presenter | viewer | – |
| multicast idle | presenter, nobody watching | idle | idle (other floor) |

**Report:**
- presenter upload: unicast with 2 viewers vs multicast (the same for any number of viewers);
- the flood cost on a same-floor laptop that isn't watching (L2) against the other floor (L3).

---

## S5 (10 min): Phase 9 UI gate, on Linux
L1 and L2 run `lancall_gui --name <me> --directory <L1 ip> --log logs/G9_<me>.csv`; L3 uses
the terminal.

- **G9.1:** using only the UI on L1:
  - call L2 from the Peers panel (L2 accepts from the popup), then add L3;
  - chat by typing `text all …` in the command box (there's no chat panel);
  - switch to "Relay on this laptop" and back to Mesh;
  - check the safety codes in the members table, then hang up.
- **G9.2:** L1 types `broadcast ui` in the command box; L2 watches with the Peers panel's
  Watch button.
- **G9.4:** a 2-minute L1 ↔ L2 call in the UI. Compare its audio/video p50 (`log_summary`)
  with the soak's first minutes, and note CPU from `top`. Pass: within ~5 ms.

## Wrap-up (10 min, after testing)
- `tools/netem.sh off` on every Linux laptop.
- Copy the experiment CSVs and key logs to `results/`; add headline rows to `summary.csv`.

## Report metrics → test

| Metric | Target | Test |
|---|---|---|
| Latency | < 300 ms (aim < 150) | E1, C1, E5 |
| Loss tolerance and repair | usable at 5% | C2, E8, E2 |
| A/V sync | < 150 ms | E1, C1 |
| Frame rate | ≥ 15 fps at 480p | C1, E5 |
| Group size | ≥ 4 | E5 shows 3 here; 4 needs the Windows laptop (W5) |
| Call setup | < 2 s | C1 |
| Discovery | < 5 s | D1 |
| Recovery after a 1 s outage | < 2 s | C5 |
| Adaptive under a cap | down < 5 s, back up | E4 |
| Jitter vs buffer | sweet spot | E3 |
| Encryption cost | µs, %, CPU | E9, E7 |
| Upload: mesh / relay / multicast | per sender | E5, E10 |
| Stability | 15 min soak | E7 |
| UI overhead | ≤ 5 ms | G9.4 |

## Features claimed → test
- Discovery: D1. Directory: the S1 cross-floor list. Old clients refused: C8.
- Calls: C1, C6. Group mesh, relay, `relay me` and a relay on a non-member: E5, G2. Dead member: G7.
- NACK: C2. FEC: E8. Adaptive video: E4. Chat (1:1 and group): C7, G4.
- Encryption and safety codes: C1, G4.
- Broadcast and watch: B1. Bridge: B3. Multicast call and auto switch: M1.
- UI: G9 (Linux).

## Cut from the full plan (not claimed as separately tested)
- C3 one-sided loss (#9 is settled offline in P3).
- C4 FEC inside a call (E8 measures FEC).
- Most of C6's life-cycle cases, C9 directory restart, and the old-binary build (C8 uses `nc`).
- E5 with 2 people.
- Busy callee, host hangup, multicast refused, two broadcasters at once.
- E10 relay runs.
- UI settings.
- 30 min soak, now 15 min.

---

## Windows (Phase 8): run once `lancall.exe` works

**Prerequisite:** on W, build the tag with MSYS2 UCRT64 from the documented steps; `ctest`
passes apart from the Linux-only tests (gate W1). Add a firewall rule (or mark the network
Private). Install a GUI build if Phase 9 builds on Windows.

**Where:** W in a floor-B room with L3. L1 and L2 are needed only for W5 and W6.

**Time:** about 25 min as a separate session.
- **Alternative, if W is ready on the day:** fold it into the main plan, which adds only
  about 10 min (≈ 1 h 55 min in total).
- **How to fold it in:**
  - W2–W4 run on L3 ↔ W during S2, alongside the soak; L3 skips E2.
  - W5 adds mesh 4 and relay 4 to E5, and runs G2b with `lancall_relay.exe` on W, instead of
    on L3.
  - W6 joins S5.

| Gate | Steps | Pass |
|---|---|---|
| W1 build | clean clone, documented steps, `ctest` | builds; tests pass apart from the Linux-only ones |
| W2 discovery + 1:1 | `list` on W and L3; W calls L3 for 2 min, then L3 calls W; chat both ways; `members` | each lists the other within 5 s (the subnet broadcast address on Windows); audio and video both ways; encrypted; same safety codes; no audio crackle (`timeBeginPeriod`); L3's log gives audio p50 |
| W3 watch | L3 `broadcast floorB`; W `watch L3` 1 min, `unwatch` | video and sound on W; `list` shows the title |
| W4 multicast call | L3 calls W, then `multicast` | the status shows `multicast 239.255.77.x` on both; media flows |
| W5 group + relay on Windows | E5 **mesh 4, relay 4** (W accepts by hand). Then L1 hosts L2 and L3, and W runs `lancall_relay.exe`; L1 `relay <W ip>`, 1 min, `mesh` | W's tile is live and W sees 3 tiles; the 4-person runs give the "≥ 4 people" metric; the relay on Windows forwards for 3 members |
| W6 UI on Windows | W runs `lancall_gui`; L1 (GUI) adds W; W accepts from the popup; W watches L3's broadcast with the Watch button | the session works on Windows, from the UI only |
| W7 directory on Windows | W runs `lancall_directory.exe`; L3 restarts with `--directory <W ip>` | peers from floor A show up on L3 |

Report rows: W2's latency (Windows ↔ Linux), W5's 4-person E5 rows, and pass/fail for W1–W7.
If Phase 8 never lands, the report states that Windows support covers the platform layer and
CI only.
