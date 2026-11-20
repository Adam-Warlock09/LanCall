# LanCall notes

Running notes for the team: setup details that belong in the README later, known issues,
and findings for the report. Add to the end of a section, and mark issues FIXED rather than
deleting them, so we can always check whether we missed something.

## Setup (README material)

- **Install:** `sudo apt install build-essential cmake git libopencv-dev portaudio19-dev clang-format libsodium-dev libglfw3-dev`
  (libsodium since phase 6, for encryption; GLFW since phase 9, for `lancall_gui`, which is
  skipped when it's missing).
  For network tests also `iperf3 netcat-openbsd nmap tcpdump`.
- **Build:** `cmake -S . -B build && cmake --build build -j`.
  **Tests:** `ctest --test-dir build/tests --output-on-failure`.
- **Formatting hook**, once per clone: `git config core.hooksPath .githooks`.
- **First run on a new laptop:** `./build/tools/devcheck` (camera, mic, speaker, link speed).
- **Receive buffer:** `sudo sysctl -w net.core.rmem_max=8388608`. Resets on reboot; to keep it,
  put `net.core.rmem_max=8388608` in `/etc/sysctl.d/99-lancall.conf`.
- **Ports:** media 5004 (video) and 5006 (audio) UDP, discovery 5010 UDP, signaling 5011 TCP,
  directory 5012 UDP, group-call relay 5014 UDP (phase 4), broadcast bridge 5016 UDP
  (phase 7). Multicast (phase 7) uses groups 239.255.77.x on the same media ports. With ufw on:
  `sudo ufw allow 5004:5016/udp && sudo ufw allow 5011/tcp`.
- **Windows (phase 8):** build with MSYS2 (msys2.org). In the "MSYS2 UCRT64" shell, once:
  `pacman -S --needed git mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,opencv,portaudio,libsodium,glfw}`.
  Then `cmake -S . -B build -G Ninja && cmake --build build` and
  `ctest --test-dir build --output-on-failure`, the same as on Linux. Run the programs from
  that shell (their DLLs are in `/ucrt64/bin`); from cmd or PowerShell, put
  `C:\msys64\ucrt64\bin` on PATH first. Run `./build/tools/devcheck` once: it shows which card
  is used (wired, not Wi-Fi), its speed and MTU, the cameras and the audio devices.
- **Windows firewall:** Windows blocks incoming traffic on networks it calls Public, which a
  hostel LAN usually is, and then nobody can call you or see you in `list`. Either make the
  wired network Private (Settings > Network > Ethernet > Private) and click "Allow" when
  Windows asks the first time, or, in an administrator PowerShell, once:
  `New-NetFirewallRule -DisplayName "LanCall UDP" -Direction Inbound -Protocol UDP -LocalPort 5004-5016 -Action Allow`
  and `New-NetFirewallRule -DisplayName "LanCall TCP" -Direction Inbound -Protocol TCP -LocalPort 5011-5013 -Action Allow`.
- **Windows differences:** the camera opens through DirectShow, audio prefers WASAPI (MME's
  default devices add ~100 ms; `--in` / `--out` still pick any device from `audio_sender
  --list`), discovery also beacons to each network's own broadcast address (255.255.255.255
  leaves by one card only there), Ctrl+C or closing the console window stops a program
  cleanly. No netem: in experiments Windows laptops are call endpoints only.
- **Network:** use a wired wall port. A personal Wi-Fi router puts the laptop behind NAT, so
  nobody can reach it and broadcast/multicast can't pass.
- **Echo:** use headphones in every call.
- **Logs:** every program defaults to a timestamped name under `logs/`; `--log PATH` overrides it.
- **Stopping:** Ctrl+C. Ctrl+Z only pauses a program and keeps the camera busy
  (`jobs`, `kill %1`; `fuser /dev/video0` shows who has the camera).
- **Loss and delay tests:** `tools/netem.sh on 5` / `off` (outgoing traffic only, so run it on
  both laptops for a two-way test). On one laptop, netem on `lo`. Always switch it off afterwards.
- **Bandwidth cap:** `tools/netem.sh cap 2` (Mbit/s, tbf with a 50 ms queue) / `off`.
  `IFACE=lo` for loopback. `DRY_RUN=1` prints the tc commands instead of running them.
- **Experiments (one laptop, loopback):** `tools/exp_e2_loss.sh` (loss vs strips),
  `tools/exp_e3_jitter.sh` (jitter vs audio buffer, headphones on), `tools/exp_e4_cap.sh`
  (bandwidth cap, adaptive vs fixed), `tools/exp_e8_repair.sh` (phase 5: loss vs nothing /
  NACK / FEC / both). Each writes `experiments/<name>_<time>/results.csv` (or per-second CSVs
  for E4). They ask for sudo once, for tc, and switch shaping off on exit. E5 (group calls)
  needs several laptops: see the top of `tools/exp_e5_group.sh`. E10 (phase 7: one presenter
  to 1-3 viewers by unicast, relay or multicast, plus what the multicast flood costs a laptop
  that isn't watching) needs 4 laptops on one floor: every laptop runs
  `tools/exp_e10_multicast.sh run <mode> <viewers> <role> <name>` with its role (presenter,
  viewer, relay, idle) and the IPs in environment variables (see the top of the script); it
  reads the wired card's counters, so no sudo. Rows go to `experiments/e10_rows_<name>.csv`.
- **"Clock skew detected" or "undefined reference" right after pulling:** the laptop clock
  moved and make trusts stale files from "the future". Check `date`, then `rm -rf build` and
  rebuild.
- **Kernel drops:** `netstat -su | grep "receive buffer errors"`; if it rises, raise `rmem_max`.
- **Harmless warnings:** `QSocketNotifier: Can only be used with threads started with QThread`
  (OpenCV's Qt window code). The GStreamer warning is avoided by opening the camera with V4L2.
- **Don't sweep the network:** a fast nmap scan of a whole /23 knocked a wall port offline.
- **Video options:** `--strips N` (1 = whole frame), `--quality`, `--fps`, `--camera`;
  `video_receiver --from IP` ignores other senders.
- **Control plane by hand:** `./build/tests/control/control_demo <name> [--port 5013]
  [--directory IP]` (a second copy on one laptop needs another port, but not 5012: that's the
  directory's). Since phase 6, `nc <ip> 5011` only gets as far as the handshake: lines after
  it are encrypted, and a plain `INVITE` is answered `DECLINE reason=old-version`.
- **Calls across floors:** run `./build/control/lancall_directory` on one laptop (any floor),
  then give everyone that laptop's IP with `--directory IP`. Peers from other floors show up
  in `list` marked as remote. Restarting the directory is harmless: the list refills in 2 s.
- **Adaptive video (phase 3):** on by default. The sender's stats line shows the step
  (`L0 15fps q70` is the top) and what the receiver reports (`peer loss`, `jitter`).
  `video_sender --fixed` turns it off, for comparing in experiment E4. The ladder ignores
  random loss up to 10% on purpose (the strips handle that); it reacts to congestion.
- **Group calls by hand (phase 4):** `control_demo` has `add <name|ip> [port]`, `members`,
  `relay <ip>` and `mesh`. On one laptop, ring `127.0.0.2`, `127.0.0.3`, ... (all of 127/8 is
  this laptop, and the host needs a different IP per member), each copy on its own `--port`.
- **Relay:** `./build/net/lancall_relay` on any laptop, in the call or not, then the host
  switches to relay mode with that laptop's IP. It learns who's in the call from the traffic
  (stream id = member id), forgets anyone silent for 5 s, and prints its members every 5 s.
  Needs UDP 5014 open.
- **Video programs with several people:** `video_sender 10.8.66.20,10.8.64.7 --id 2` sends
  to both (mesh by hand); `video_sender <relay_ip>:5014 --id 2` goes through a relay.
  `video_receiver --id 3 --from <ips or relay ip>` opens one window per stream.
- **Loss repair (phase 5):** NACK retransmission is always on (receivers ask, senders and the
  relay resend; `video_sender --no-nack` turns the resending off for E8). FEC is off unless
  asked for: `lancall --fec` or `video_sender --fec` adds a parity packet after every strip
  (~1 packet in 3 more upload). The receiver's stats line shows `nack N  fec M` per second,
  the sender's `resent N`, the relay's `resent N` (answered from its own copy).
- **Encryption (phase 6):** always on, nothing to set. Every signaling connection (calls and
  messages) starts with a key exchange and is encrypted after it; the host makes a random key
  per call and media is encrypted with it (the header stays readable, so the relay still
  works without the key). lancall says "in call with X (audio+video, encrypted)" and prints a
  safety code ("1234 5678") for each connection you share: read it to the other person, and
  if the codes differ someone is in between. `members` shows the codes again. Everyone needs
  a phase 6 build: an older one is turned away with a clear message. Cost:
  `./build/tools/crypto_bench` (experiment E9).
- **Text chat (phase 6):** in lancall, `text all <msg>` goes to everyone in your call and
  `text <name|ip> <msg>` to one person, in your call or not (a member of the call by name
  first, then `list`). Incoming: `[chat] Atul: ...`, or `(to you)` when private; a message
  that can't be delivered says so. Up to 500 bytes. In a group call the host passes messages
  on, so the host's laptop sees private ones between two members too. The log keeps counts
  (`chat_sent`, `chat_received`), never the text. By hand: `control_demo` has
  `text all <msg>` and `text <ip>[:port] <msg>`.
- **Broadcast (phase 7):** in lancall, `broadcast <title>` sends your camera and mic once to a
  multicast group (239.255.77.x, picked so it doesn't clash with anyone else's on the floor)
  and announces it in your discovery beacon; `unbroadcast` stops it. Everyone on the floor
  sees it in `list` (`broadcasting "title" (watch Atul)`) and types `watch Atul`; `unwatch`
  stops. No call setup, and viewers send no media. Fixed video level, FEC on, no NACKs, not
  encrypted. Only your own floor can watch (multicast stops at the floor router), and the
  stream reaches every laptop on the floor, watching or not (no IGMP snooping). Calls,
  broadcasting and watching exclude each other. One laptop: `lancall --name A --mc-loop` and
  `lancall --name B --port 5013` in two terminals (headphones), `broadcast test` in A,
  `watch A` in B.
- **Multicast calls (phase 7):** in a call, the host types `multicast` (or types it before
  calling, for the next call): everyone sends one copy of their audio and video to a group
  and joins it, instead of N-1 copies in mesh. Only when discovery hears every member on the
  host's floor; otherwise it's refused. If someone from another floor joins later, the host
  switches the call to a relay on its own laptop by itself. Still encrypted with the call's
  key, so the other laptops on the floor (which get the flooded packets too) can't read it.
  `members` and the status line show `multicast 239.255.77.x`. Needs two laptops at least:
  two lancalls on one laptop can't both bind the media ports.
- **Bridge (phase 7): a broadcast on another floor.** Multicast stops at the floor router, so
  on its own a broadcast reaches only its floor. To show it on floor B: the presenter (while
  broadcasting) types `bridge <ip of a laptop on floor B>`, which prints the line to run on
  that laptop: `./build/net/lancall_bridge --from <presenter ip> --name <presenter> --title
  "<title>"`. The bridge takes one unicast copy on UDP 5016 (about 5 Mbps more upload for the
  presenter per bridge), multicasts it on floor B and announces it there as `<name>-bridge`
  with the title plus "(via bridge)", only while the presenter's media arrives. Viewers on
  floor B type `watch <name>-bridge`. It passes on only the presenter's media (nobody else's),
  sends viewers' reports back to the presenter, drops NACKs. `unbridge <ip>` stops the copy.
  The bridge laptop can watch too, with its own lancall.
- **Test length:** 1–2 minutes per test from phase 4 on. Long enough for the ladder (5 s up,
  40 s worst-case probe wait) and a few hundred latency samples, short enough to repeat.
- **Plot data:** anything we'll want in the report goes in `results/` (committed, unlike
  `logs/` and `experiments/`). See `results/README.md`.
- **`lancall_gui` aborts at start with "GLib-GIO-ERROR ... does not contain a key named
  'antialiasing'":** it was started from the terminal of VS Code installed as a snap, which
  hands its own GTK settings to everything started there (GLFW draws the window frame with
  GTK on Wayland). Run it from a normal terminal (Ctrl+Alt+T) instead.
- **The window app (phase 9):** `./build/gui/lancall_gui [--name N]` takes lancall's
  options except `--peer` (direct mode stays in the terminal app); without `--name` a start
  screen asks for it. Left: **Peers** (Call, Add while you host, Watch a broadcast, call by IP)
  and **Call** (answer in the popup, members with safety codes, mesh / relay / multicast for
  the host, start a broadcast, bridges, hang up). Right: the video, laid out like lancall's
  window. Below, tabs: **Messages** (everything lancall would print, plus a box that takes any
  lancall command), **Chat** ("Chat (2)" = unread), **Stats** (live, with Copy), **Settings**
  (microphone, speaker, camera, buffer, fps, quality, FEC from the next call; the directory
  when idle). Settings aren't saved: they last until the window closes. Logs are
  `logs/lancall_gui_<name>_<date>_<time>.csv` with the same rows as lancall, so `log_summary`
  works as usual. It's the same call code as the terminal app (`call/app.hpp`), which stays:
  the experiment scripts drive `lancall`. Run it from a normal terminal (see the previous
  bullet).

## Known issues

| # | Status | Issue | Owner |
|---|---|---|---|
| 1 | FIXED (aric/tools-experiments) | `log_summary` resets its received-packet counts when a sender restarts (`restarts 1`), so a log with two calls shows only the last call's received stats (about 6 s in the Phase 2 run). The latency timings cover the whole file. Fix: `StreamStats::segments()` keeps each run; `log_summary` prints one line per call. | Atul |
| 2 | FIXED (aric/tools-experiments) | Runs that exit straight away leave logs with 0 rows, which `log_summary` still listed. Now skipped, with one note at the end. | Atul |
| 3 | DONE | Phase 2 gate leftovers: two-laptop call and 5% loss call, run as tests A1 and A2 in the phase 3 session. | both |
| 4 | FIXED (aric/tcp-nodelay) | Signaling sockets lacked `TCP_NODELAY`. Nagle + delayed ACK held a PING or PONG for ~40 ms: `rtt_ms` ~41 on one laptop (0.7 on the other) and that laptop's e2e latencies ~17 ms too high. | Aric |
| 5 | FIXED (phase 1) | `video_receiver` called `waitKey` after every packet, fell behind at ~500 packets/s and the socket buffer overflowed, silently dropping whole strips. Fix: drain all waiting packets first, and count missing strips from frame-ID gaps. | Aric |
| 6 | FIXED (phase4-kickoff) | Video jitter included the sender's own wait: frames sit 0–33 ms before the send thread picks them up, and the header carries the capture time. The receiver saw ~10–16 ms of fake jitter on a clean network, so the ladder took any loss as "loss + jitter" and stepped down (B2, Atul's side dropped to L4 at 5% random loss), and never stepped back up (E4: stuck at L4 after the cap was lifted; A3 the same). Fix: each strip carries its capture→send delay (`video/strip.hpp`, 8-byte prefix now) and jitter is computed from send time. | Aric |
| 7 | FIXED (phase4-kickoff) | A PING or PONG that TCP had to resend (under netem loss) has a round trip of 200+ ms and an offset that's off by hundreds of ms. At the start of a call it can be the only sample, so e2e latency read −715 ms min, rtt 1577 ms (A2). Fix: the clock filter drops samples with rtt > 100 ms. | Aric |
| 8 | FIXED (atul, phase 4), not yet verified on hardware | Audio playout delay ratchets up: the buffer only trims when it holds more than prebuffer + 5 chunks, so after any burst it stays up to 160 ms deep for the rest of the call. B1: audio e2e p50 154 ms vs 74 ms in A1, video followed (lip sync). Fix: if the depth stays above prebuffer + 1 for 1 s (50 chunks in a row), drop one chunk, then count again (`PlayoutBuffer`, `drift_trims` in the stats line and the log). Re-check B1's audio e2e p50 (~74 ms expected). | Atul (phase 4) |
| 9 | open | A2 measured 7–13% loss with netem set to 5%, plus one ~1.5 s stall. Maybe netem on both laptops counted twice for reports (each side's outgoing loss shows on the other) plus the stall, maybe something else. Re-run a 2-minute A2 once #6–#8 are merged and compare `netem.sh status` on both laptops. | both |
| 10 | FIXED (aric/phase5-nack) | A member whose cable is pulled (or who sleeps or crashes hard) never closes TCP, so the host kept them in the call with a frozen tile for ~15 minutes. Now 10 s without a single line (both sides PING every 2 s) counts as gone: reason "no response". | Aric |

## Findings (for the report)

**Network (Nov 12–13 tests).** Each floor is its own /23 with its own gateway, one router hop
between floors. TCP and UDP work everywhere; broadcast and multicast stay on one floor
(multicast is flooded to every port: no IGMP snooping). RTT ~1.2–1.4 ms, jitter ≤ 0.04 ms,
0% UDP loss. Throughput was limited by the USB adapters (~95 Mbps on a 100 Mbps adapter,
~300 Mbps on gigabit ones, typical of USB 2.0). MTU 1500. A VXLAN tunnel carried broadcast
across floors. nmap: ~20 /23 networks behind one central router; 10.8.76.0/23 sits behind a
second router.

**Phase 1: whole frames vs strips (E2 preview, 5% netem on loopback).** Whole JPEG frames
(~23 packets each) arrived complete only ~27% of the time (theory 0.95^23 ≈ 31%), giving
2–7 fps. Ten strips per frame (~3.3 packets each) kept 12–15 fps for ~22% more bandwidth.

**Phase 2: where the ~80 ms goes (start of E1).** Audio spends 20 ms filling a chunk, 20–40 ms
in the playout buffer, some time in the sound device, and ~1 ms on the network. Video needs
~19 ms to capture, encode and send, and then lip sync deliberately holds each frame until the
audio catches up. So video latency is set by audio, by design, and shrinking the audio buffer
lowers both. Measured: audio e2e median ~74–82 ms, video ~82–89 ms (self-calls, exact shared
clock; the two-laptop call matched once the clock-sync bias was understood), A/V offset
−8 ms (video slightly behind audio), 15 fps, 0% loss, clock-sync rtt 0.7 ms.

**Clock offset between laptops.** `now_us()` counts from boot, so two laptops' clocks differ by
their boot gap (~1721 s in our tests). Opposite-direction transit times give it directly:
offset = half their difference, one-way delay = half their sum (~0 ms here). Signaling's
PING/PONG does the same automatically, modulo 2^32.

**Phase 3 tests.** Clean two-laptop call (A1): audio e2e p50 ~74 / 90 ms (the two
directions), video ~81 / 96 ms, clock-sync rtt 0.6–0.7 ms, 15 fps. Under a 2 Mbit/s cap (A3)
the sender stepped down within ~0.1 s of the first bad report, but never back up (issue #6).

**E2: loss vs strips.** With 10 strips the receiver kept 12.4–13.9 fps all the way to 20%
loss. Whole frames collapsed already at 5% (3.0 and 7.8 fps on the two laptops). Data in
`results/e2_loss.csv`.

**E4: bandwidth cap (first run).** Adaptive sender: level 0 → 2 at 15.1 s, → 4 at 16.7 s (cap
on at 15 s), so the 5 s gate passed. It stayed at 4 after the cap went off at 40 s: that was
issue #6, so E4 needs a re-run with the fix. Data in `results/e4_cap_levels.csv`.

**E6: across floors (B1, B2).** Calls between floors (one router hop) work as well as on one
floor: same ~0.6 ms clock-sync rtt, no extra loss. The higher latency in B1 was the audio
buffer (issue #8), not the network. So for this hostel the second floor costs nothing
measurable; discovery is the only thing that needed help (the directory).

## Plan from phase 4 on

- **Phase 4: group calls.** Rooms in signaling (host = id 1), mesh and relay (`net/relay`,
  `lancall_relay`), several streams per receiver, audio mixing, grid view. Experiment E5:
  upload per sender and latency for 2, 3, 4 people, mesh vs relay.
- **Phase 5: recovering loss.** NACK retransmission for video strips (ask at once, once more
  after 10 ms, give up after 80 ms; a frame missing strips waits up to 25 ms for repairs; the
  relay answers from its own copy) and FEC (one XOR parity packet per strip). Experiment E8:
  none / NACK / FEC / both at 5, 10, 20% loss.
- **Phase 6 (Aric): text chat and encryption.** `text all <msg>` / `text <name|ip> <msg>`
  over signaling (TCP; in a call the host passes messages on, outside a call a short
  connection of its own). Key exchange when two lancalls connect (libsodium), a random key
  per call from the host, every media datagram sealed (header readable but tamper-proof, so
  the relay still forwards it), safety codes to compare. Experiment E9: what it costs.
- **Phase 7 (Atul): multicast.** Broadcast mode (one presenter, any number of viewers on the
  floor, one upload copy; announced in beacons, `watch <name>`), a multicast mode for group
  calls when everyone is on one floor, and a bridge re-multicasting a broadcast on another
  floor (`lancall_bridge`, done).
  Multicast stays on the floor and is flooded to every port there (no IGMP snooping).
  Experiment E10: unicast fan-out vs relay vs multicast, plus the flooding's cost to a laptop
  that isn't watching.
- **Phase 8 (Aric): Windows support.** The kickoff added `common/platform.hpp` (frozen):
  socket headers, `init()`, `on_interrupt()`, `LineReader`, `local_time()`, `process_id()`,
  our own / outgoing address, `StderrSilencer`; Linux code moved there, the Windows half is
  this phase. Rule from now on: no POSIX or Windows header (`<unistd.h>`, `<poll.h>`,
  `<sys/...>`, `<arpa/inet.h>`, `<netinet/in.h>`, `<csignal>`, `<windows.h>`) outside
  `common/platform.*`, `common/udp_socket.cpp`, `control/signaling.cpp`,
  `tools/devcheck.cpp` and `tests/sock_compat.hpp` (raw sockets for tests that drive the
  wire by hand). Work: the Windows half of platform.cpp; Winsock in the socket class
  and signaling (loopback UDP pair instead of the wake-up pipe, `ioctlsocket` instead of
  `fcntl`, no `MSG_NOSIGNAL`); discovery broadcasting to the subnet's own broadcast address
  (255.255.255.255 leaves by one card only on Windows); MSMF/DirectShow instead of V4L2;
  WASAPI over PortAudio's default MME; `devcheck` fallbacks; the relay and directory programs
  and the rest of Aric's files onto `platform::on_interrupt`; build with MSYS2 UCRT64 (same
  GCC, pkg-config works); GitHub Actions on Ubuntu and Windows. Watch for: 15.6 ms sleeps
  without `timeBeginPeriod(1)`, `IP_MULTICAST_LOOP` applying to the receiving socket on
  Windows, `SO_REUSEADDR` meaning something else there, the firewall blocking inbound on
  networks marked Public. Experiments stay Linux-only (netem); Windows laptops are only call
  endpoints in them. Gate: clean clone builds from written steps, ctest passes (minus
  Linux-only tests), Windows <-> Linux 1:1 call with audio, video, encryption and chat,
  Windows in a 3-person mesh and relay call, watching a broadcast, in a multicast call;
  `lancall_relay` and `lancall_directory` run on Windows.
- **Phase 8 test on a real Windows laptop** (the gate; wired port, firewall set up as in
  Setup, headphones), each 1-2 minutes, with a Linux laptop on the same floor:
  1. `devcheck`: wired card picked, 1500 MTU, camera at 640x480 and ~15 fps, mic level, tone.
  2. Each sees the other in `list`; Windows calls Linux, then Linux calls Windows: audio and
     video both ways, "encrypted", the same safety code on both, `text` both ways,
     `hangup`. Check `stats`: no steady loss, audio e2e close to a Linux-Linux call (WASAPI).
  3. A third laptop: 3-person call with Windows as host and as member, `relay me` on Windows,
     then `relay <linux ip>`, then `mesh`.
  4. Windows watches a Linux broadcast; then Windows broadcasts and Linux watches; a
     `multicast` call with Windows in it.
  5. `lancall_relay` and `lancall_directory` on Windows, used by the Linux laptops.
  6. Ctrl+C in each program, and closing a console window mid-call: the other side sees the
     call end (at most 10 s for the window), and the log file is complete.
  7. `lancall_gui` on Windows, once phase 9 is in.
- **Phase 9 (Atul): UI.** `lancall_gui` (`gui/`), Dear ImGui (copied into `third_party/imgui`,
  see `third_party/README.md`) on GLFW + OpenGL 3, main thread, ~60 Hz; built only when GLFW
  is found (`-DLANCALL_GUI=OFF` skips it anyway). First pull lancall's state and actions out of
  `call/main.cpp` into an app core that the terminal and the GUI both drive. Panels: peers
  (call, watch), call (incoming popup, members with safety codes, mesh/relay/multicast, add),
  grid (a texture per stream, self view), chat, stats, settings (devices, buffer, fps,
  quality, FEC, directory). Lip sync keeps choosing the frame. The terminal `lancall` stays:
  the experiment scripts use it. GUI code stays portable (only `common/platform.hpp`). Gate:
  a 3-person session done only from the GUI (call, chat, switch to relay, watch a broadcast),
  latency within ~5 ms of the terminal app; on Windows too, checked together once phase 8
  lands. (Built; the gate session itself moved to phase 10, see "Hardware tests left for the
  testing phase" below.)
- **Who touches what in phases 8-9:** Aric: `common/`, `net/` (not the bridge), `control/`,
  `video/`, `tools/`, CI. Atul: `call/`, `audio/`, `metrics/`, `gui/`, `third_party/`,
  `net/*bridge*`. Shared, append-only: the top-level `CMakeLists.txt`, `NOTES.md`.
- **Phase 10:** E7 (long run / soak), every experiment re-run on the final build, the parked
  phase 4/5 tests, report, demo.

## Hardware tests left for the testing phase

Everything below is built and unit-tested but still needs real laptops. Run it in phase 10,
on the final build, before the experiment re-runs (a bug found here changes their numbers).

### Phase 9 gate

The window app (`lancall_gui`) is built, but its gate session is left for phase 10, with the
other hardware tests. What was already checked on one laptop: a call rings the GUI and the
popup answers it; chat both ways; the GUI broadcasting to a terminal viewer and watching a
terminal presenter (15 fps, 0% loss); the terminal lancall prints exactly what it did before
phase 9 (same typed script, same option errors). Still to do, on three laptops on one floor
(P runs `./build/gui/lancall_gui --name P`, Q and R the terminal lancall; wired, headphones,
about 25 minutes):

- **A. Everything from P's window, no typing in its command box:**
  1. Peers -> Call Q (Q `accept`): both see each other, P's safety code matches Q's `members`.
  2. Peers -> Add R (R `accept`): a grid of three named tiles, a code for Q and for R.
  3. Chat tab: to everyone in the call, then privately to R; Q and R answer with `text P hi`
     (the Chat tab counts them as unread while another tab is open).
  4. Call panel: Relay on this laptop, then Multicast, then Mesh; media keeps going each time.
  5. Hang up; Q `broadcast demo`; P Peers -> Watch Q (P sees Q), then Stop.
  6. P Start broadcast (Call panel); Q and R `watch P`; P Stop broadcasting.
- **B. Latency, the same P-Q call twice for 2 minutes, P's side:** run T with P on
  `./build/call/lancall --name P --log logs/p9_T.csv`, run G with P on
  `./build/gui/lancall_gui --name P --log logs/p9_G.csv`; Q unchanged. Then
  `./build/tools/log_summary logs/p9_T.csv logs/p9_G.csv`, plus CPU from
  `top -b -n 2 -d 10 | grep lancall | tail -1` about 1.5 minutes into each run.
- **Passes when:** every step in A works without P typing a command, and run G's video e2e p50
  is within 5 ms of run T's. Audio e2e can differ by 10-20 ms between any two runs (the
  playout buffer settles at 40 or 60 ms), and the GUI can't change audio, so if audio moved,
  judge by the A/V offset (`av_sync_ms` p50, video minus audio) instead: within 5 ms.
- **Record:** one row per run in `results/p9_gui_gate.csv` (columns in `results/README.md`),
  the headline numbers in `results/summary.csv`, and a paragraph in Findings with A's
  results. The Windows half of the gate follows once phase 8 is merged.

### Phase 7: multicast

Already seen working on one laptop during the phase 9 checks: a broadcast and a watch, both
ways round (lancall_gui presenting to a terminal viewer and the other way), 15 fps, 0% loss,
with `--mc-loop` and `--port 5013`. Still to do:

- **Multicast call (2-3 laptops, one floor, ~10 min):** call, `add` the third, then the host
  types `multicast`. `members` and the status line show `multicast 239.255.77.x`; audio and
  video keep going; `stats` on each laptop shows media arriving from every member. Back with
  `mesh`, then `multicast` again. Also: type `multicast` before calling (the next call starts
  in it). Passes when nothing stalls across the switches and every member hears and sees the
  others.
- **Multicast call with someone off the floor (2 floors, a directory):** host and one member on
  floor A in multicast mode, then the host adds someone from floor B (`--directory` on
  everyone). The host should switch the call to a relay on its own laptop by itself
  ("... not on this floor ... switching to a relay on this laptop"), and everyone keeps media.
  `multicast` typed again should be refused, naming the floor-B member.
- **Bridge across floors (2 floors, 3 laptops, ~10 min):** P on floor A types
  `broadcast demo`, then `bridge <ip of B1>`, and runs the line it prints on B1 (floor B).
  B2 on floor B: `list` shows `<P>-bridge` broadcasting "demo (via bridge)"; `watch
  <P>-bridge` shows P. B1's bridge prints its rate every 5 s (forwarded, reports back). Then P
  `unbridge <ip of B1>`: within ~5 s B1 says "announcement off" and B2's watch ends by itself.
  Passes when B2 sees P at 15 fps and the announcement follows the media both ways.
- **E10 (4 laptops, one floor, ~30 min):** the 9 runs (unicast / relay / multicast x 1, 2, 3
  viewers, ~90 s each) as the top of `tools/exp_e10_multicast.sh` says, with the spare laptop
  as role idle in the 1- and 2-viewer runs. Collect into `results/e10_multicast.csv`, headline
  numbers into `results/summary.csv`. What to look for: the presenter's upload staying flat
  with multicast as viewers grow, and the idle laptop's `nic_rx_kbps` (the flood's cost).

### Older ones parked until now

- **Issue #8 (audio buffer ratchet, fixed in phase 4):** a clean two-laptop call across floors
  like B1, 2 minutes. Audio e2e p50 should be ~74 ms (not 154 ms); `stats` shows
  `drift-trimmed` counting now and then.
- **Issue #9 (A2 loss reading 7-13% at 5% netem):** a 2-minute A2 with `tools/netem.sh on 5`
  on both laptops, `tools/netem.sh status` on both before starting; compare the loss each side
  reports with 5%.
