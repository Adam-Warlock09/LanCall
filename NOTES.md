# LanCall notes

Running notes for the team: setup details that belong in the README later, known issues,
and findings for the report. Add to the end of a section, and mark issues FIXED rather than
deleting them, so we can always check whether we missed something.

## Setup (README material)

- **Install:** `sudo apt install build-essential cmake git libopencv-dev portaudio19-dev clang-format`.
  For network tests also `iperf3 netcat-openbsd nmap tcpdump`.
- **Build:** `cmake -S . -B build && cmake --build build -j`.
  **Tests:** `ctest --test-dir build/tests --output-on-failure`.
- **Formatting hook**, once per clone: `git config core.hooksPath .githooks`.
- **First run on a new laptop:** `./build/tools/devcheck` (camera, mic, speaker, link speed).
- **Receive buffer:** `sudo sysctl -w net.core.rmem_max=8388608`. Resets on reboot; to keep it,
  put `net.core.rmem_max=8388608` in `/etc/sysctl.d/99-lancall.conf`.
- **Ports:** media 5004 (video) and 5006 (audio) UDP, discovery 5010 UDP, signaling 5011 TCP,
  directory 5012 UDP. With ufw on: `sudo ufw allow 5004:5012/udp && sudo ufw allow 5011/tcp`.
- **Network:** use a wired wall port. A personal Wi-Fi router puts the laptop behind NAT, so
  nobody can reach it and broadcast/multicast can't pass.
- **Echo:** use headphones in every call.
- **Logs:** every program defaults to a timestamped name under `logs/`; `--log PATH` overrides it.
- **Stopping:** Ctrl+C. Ctrl+Z only pauses a program and keeps the camera busy
  (`jobs`, `kill %1`; `fuser /dev/video0` shows who has the camera).
- **Loss and delay tests:** `tools/netem.sh on 5` / `off` (outgoing traffic only, so run it on
  both laptops for a two-way test). On one laptop, netem on `lo`. Always switch it off afterwards.
- **Kernel drops:** `netstat -su | grep "receive buffer errors"`; if it rises, raise `rmem_max`.
- **Harmless warnings:** `QSocketNotifier: Can only be used with threads started with QThread`
  (OpenCV's Qt window code). The GStreamer warning is avoided by opening the camera with V4L2.
- **Don't sweep the network:** a fast nmap scan of a whole /23 knocked a wall port offline.
- **Video options:** `--strips N` (1 = whole frame), `--quality`, `--fps`, `--camera`;
  `video_receiver --from IP` ignores other senders.
- **Control plane by hand:** `./build/tests/control/control_demo <name> [--port 5013]
  [--directory IP]` (a second copy on one laptop needs another port, but not 5012: that's the
  directory's); `nc <ip> 5011` then type `INVITE name=Test`.
- **Calls across floors:** run `./build/control/lancall_directory` on one laptop (any floor),
  then give everyone that laptop's IP with `--directory IP`. Peers from other floors show up
  in `list` marked as remote. Restarting the directory is harmless: the list refills in 2 s.
- **Adaptive video (phase 3):** on by default. The sender's stats line shows the step
  (`L0 15fps q70` is the top) and what the receiver reports (`peer loss`, `jitter`).
  `video_sender --fixed` turns it off, for comparing in experiment E4. The ladder ignores
  random loss up to 10% on purpose (the strips handle that); it reacts to congestion.
- **"Clock skew detected" or "undefined reference" right after pulling:** the laptop clock
  moved, and make trusts stale files from "the future". Check `date`, then `rm -rf build`
  and rebuild.

## Known issues

| # | Status | Issue | Owner |
|---|---|---|---|
| 1 | open | `log_summary` resets its received-packet counts when a sender restarts (`restarts 1`), so a log with two calls shows only the last call's received stats (about 6 s in the Phase 2 run). The latency timings cover the whole file. Fix: split the stats per call. | Atul |
| 2 | open (cosmetic) | Runs that exit straight away leave logs with 0 rows, which `log_summary` still lists. | Atul |
| 3 | open | Phase 2 gate leftovers: the 5-minute two-laptop call and the 5% loss call were not run. Do them in the first Phase 3 session. | both |
| 4 | FIXED (aric/tcp-nodelay) | Signaling sockets lacked `TCP_NODELAY`. Nagle + delayed ACK held a PING or PONG for ~40 ms: `rtt_ms` ~41 on one laptop (0.7 on the other) and that laptop's e2e latencies ~17 ms too high. | Aric |
| 5 | FIXED (phase 1) | `video_receiver` called `waitKey` after every packet, fell behind at ~500 packets/s and the socket buffer overflowed, silently dropping whole strips. Fix: drain all waiting packets first, and count missing strips from frame-ID gaps. | Aric |

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