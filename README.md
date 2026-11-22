# mt7986-qdma-autorate

Hardware download shaping for the GL.iNet GL-MT6000 (Flint 2, MediaTek MT7986) on OpenWrt 25.12.6.

## What it is

I moved download bufferbloat control on my Flint 2 router into the network chip's own hardware. The router measures
latency passively from normal traffic, with no pings or scripts, and a small controller adjusts the chip's built-in speed
limiter on the fly. On my 300 megabit cable line it gets about 335 megabits, where CAKE gets about 285, with the same low
latency. It uses almost no CPU, starts at boot, and recovers on its own from crashes.

At first a single big download on its own was slower than with CAKE, because the controller cut too far on a lone
connection. Setting the floor just under my line's normal speed (285 on the 300 line) fixed that: a single download now
gets about 267 megabits, where CAKE gets about 244.

It only touches downloads, which is where cable bloat really hurts. Upload stays on regular CAKE shaping, untouched.
Software flow offloading stays on too, and the latency tracking still sees the offloaded traffic, so I didn't have to give
up offloading. My patched CAKE with the TCP window clamp (cake-wnd) is still installed. With download handled in hardware
it's mostly idle, but if I switch back to plain CAKE it goes right back to work.

## Results

| | Download | Upload | Latency added on download |
|---|---|---|---|
| CAKE (SQM, download + upload) | about 285 Mbit/s | about 28 | about +5 ms |
| qdma-autorate (download), CAKE (upload) | about 335 Mbit/s | about 28 | LibreQoS grade A (+8.6 ms), Waveform A+ (+2 ms) |

The line is 300/30 DOCSIS cable. With no shaping at all, download latency went up by about 205 ms. (November 2026)

**v5 measured (same line):** 8-stream load, 329 Mbit/s, ping p99 38.6 ms, 0 watchdog trips. Single stream, 247–280 Mbit/s
depending on CDN path, 0 watchdog trips. Idle no longer collapses to the floor. Missing ePPing samples under load drive
the rate to the floor within 2.06 s, and recover once samples resume.

## Requirements

- **Router:** GL.iNet GL-MT6000 (MT7986).
- **Firmware:** OpenWrt **25.12.6** exactly (`mediatek/filogic`). The kernel module is tied to the exact kernel build
  (vermagic `6.12.112 SMP mod_unload aarch64` and the kernel package hash). Any other release means rebuilding both
  packages with that release's SDK (see [Building](#building)).
- **Packages:** `kmod-sched-cake` (a cake-wnd build that provides `sch_cake` works too) and SQM (`sqm-scripts`) with
  upload shaping configured.
- **LAN:** wired clients on the LAN ports. Wi-Fi traffic does not go through the QDMA queues this uses.

## How it works

- **Measuring:** a tcx BPF program on the WAN interface (ePPing, patched; see [NOTICE](NOTICE)) measures per-flow TCP
  round-trip time passively from normal traffic, including from pure ACKs. ePPing's RTT-aggregation maps are capped at
  1 entry (aggregation is unused here), fixing a ~0.5 GB fixed preallocation that otherwise sits in `Percpu`/`vmalloc`.
- **Controlling:**
  - The controller (`qdma-autorate`) lowers the rate when the median per-flow delay rises above 10 ms.
  - It raises the rate only while the LAN queue actually has a backlog, in 10 Mbit/s hardware steps: one step at most every
    500 ms, or faster back to just below the rate in use before a cut.
- **Enforcing:** the kernel module (`kmod-qdma-rl`) moves the LAN ports' QDMA TX queues onto one hardware scheduler and sets
  that scheduler's rate limit. The scheduler cap covers the LAN queues only; the WAN (upload) queue is never touched.
- **Queueing:** `cake unlimited` runs on the LAN TX queues of the switch conduit (`eth0`), so the queue that forms in front
  of the hardware limiter is managed by CAKE.
- **Offload:** software flow offload stays on.

## Install

Download the two `.apk` files and `sha256sums` from the release, copy them to the router (wired LAN) and run:

```sh
cd /tmp && sha256sum -c sha256sums
apk add --allow-untrusted /tmp/kmod-qdma-rl-*.apk /tmp/qdma-autorate-*.apk
```

The package enables and starts the service when it is installed. While the service runs, it sets SQM's download rate to
`0`, so SQM keeps only its upload shaping. It saves the original value in `/etc/config/qdma-autorate`
(`sqm_download_orig`) and puts it back when the service is stopped.

**Switch back to plain SQM/CAKE** (restores SQM's download rate, the default `eth0` qdisc and every QDMA register):

```sh
service qdma-autorate stop && service qdma-autorate disable
```

**Switch on again:**

```sh
service qdma-autorate enable && service qdma-autorate start
```

**Uninstall** (switch back first, so SQM's download rate is restored):

```sh
service qdma-autorate stop && service qdma-autorate disable
apk del qdma-autorate kmod-qdma-rl
```

**Check that it runs:**

```sh
cat /sys/module/qdma_rl/parameters/reg       # mode=scheduler capped=1 rate_mbps=...
logread -e qdma-autorate                     # warnings and errors only
```

## Safety

- **If the controller dies:** the kernel module has a watchdog. If no rate is written for 2 s, it puts back the original
  register values (uncapped). procd restarts the controller after 5 s, and it caps again.
- **Stopping the service** restores every register the module touched, unloads it and restores the default `eth0` qdisc.
- **A WAN reconnect** (`ifup wan`) restarts the service through a hotplug script.
- **After a sysupgrade** the packages are gone, but `/etc/config` is kept. `sqm download` may therefore stay `0` until you
  reinstall (rebuilt for the new kernel) or set it back by hand:

  ```sh
  uci set sqm.eth1.download=$(uci get qdma-autorate.main.sqm_download_orig); uci commit sqm; /etc/init.d/sqm restart
  ```

  (`eth1` is the SQM section name on the GL-MT6000; check yours with `uci show sqm`.)

## Known limitations

- **The floor is a tradeoff:** set `--floor` just under what your line reliably delivers (285 on a 300 Mbit/s line). With
  a low floor (150) a single bulk download only got about 140 Mbit/s, because the controller cut too far on a lone flow. With
  the floor at 285 it gets about 267, against about 244 with CAKE. The cost: the controller can never go below the floor. If
  the line drops below it, for example at busy hours on cable, latency rises again until the line recovers. A fixed CAKE rate
  just under the line speed has the same exposure.
- **Idle:** v5 no longer collapses to the floor at idle; the rate holds while the load gate is closed (`noload`) and ramps
  up within a few seconds once traffic starts.
- **UDP/QUIC:** UDP/QUIC traffic gives no RTT samples. Only TCP flows drive the controller.
- **Testing:** tested on one router and one ISP only (300/30 DOCSIS cable).
- **No XDP:** native XDP on the WAN interface (`eth1`) resets the link on this hardware, so this project uses tcx.
- **Wired only:** Wi-Fi and software/WED-offloaded traffic doesn't pass through the QDMA queues this shapes.
- **PPPoE is not supported.**

## Settings

Everything is in `/etc/config/qdma-autorate`. The controller flags are in `option args`:

```
config qdma-autorate 'main'
	option enabled '1'
	option wan 'wan'
	option lan_dev 'eth0'
	option queues '3,4,5,6,8'        # QDMA queues of the LAN ports (DSA port + 3)
	option watchdog_ms '1000'
	option args '--cir 300 --cut-integral-ms 1.0 --load-gate 0.5 --stale-ms 2000 --refresh-ms 300'
```

Restart after a change: `service qdma-autorate restart`.

**v5:** `watchdog_ms=1000` is now the recommended setting (300 ms refresh keeps the module fed well inside the watchdog
window; measured zero trips under load). `--cir` is the simplest way to set start/floor/ceil together; the flags below it
are the v5 built-in defaults shown explicitly, so you don't need to pass them unless you want a different value.

**Set the floor for your line.** The package ships with `--floor 150` as a safe start. The example above is for a 300 Mbit/s
line and uses `--floor 285`. The floor should sit just under the download speed your line reliably delivers (see Known
limitations for the tradeoff).

| Flag | Meaning |
|---|---|
| `--start` | rate in Mbit/s at start |
| `--floor` | lowest rate the controller will set; put it just under what your line reliably delivers |
| `--ceil` | highest rate (a safety bound; the rate only rises while the LAN queue is backlogged) |
| `--cut-thresh` | cut the rate when the median per-flow delay exceeds this many ms |
| `--inc-thresh` | only raise the rate while the delay is below this many ms |
| `--dec` | factor for a cut (0.95 = −5%) |
| `--probe-step`, `--probe-interval-ms` | slow increase: this many Mbit/s at most once per interval |
| `--fast-step` | increase per 100 ms tick when recovering after a cut |
| `--csv PATH` | write a per-tick CSV for debugging (off by default) |
| `--cir MBPS` | committed rate: floor = CIR × `--floor-pct` (0.95), ceil = CIR × `--ceil-pct` (1.25), start = CIR unless given; explicit `--floor`/`--ceil`/`--start` win |
| `--cut-integral-ms` | cut only once the excess delay above `--cut-thresh`, integrated over time, reaches this many ms·s; ignores short splashes (default `1.0`, 0 = cut on any tick above) |
| `--load-gate` | act only while the 1 s WAN rx rate is at least this fraction of the cap, else hold (default `0.5`, 0 = off) |
| `--stale-ms` | loaded but no ePPing RTT sample for this long → floor, until samples resume (default `2000`, 0 = off) |
| `--refresh-ms` | minimum interval between rate writes to the kernel module (default `300`) |
| `--log PATH` | write a once-a-second rate/RTT line (`rate`, `rtt_ms`, `delay_ms`, `rx_mbps`, `flows`, `ceil`, `action`), rotating at 1 MB with one old file kept (`-` = off) |
| `--ceil-creep` | after 5 continuous minutes pinned at the ceiling with delay under `--cut-thresh`, raise the ceiling by 2%; never below the configured ceiling; off by default |

`qdma-autorate -h` lists every flag.

**Example for a gigabit line** (not tested by me): the floor just under what the line reliably delivers (here about 900),
the ceiling a bit above the speed you actually get:

```
	option args '--start 900 --floor 880 --ceil 1000 --cut-thresh 10 --inc-thresh 5'
```

The hardware encodes rates as MAN × 10^EXP kbit/s: 1 Mbit/s steps up to 127, 10 Mbit/s steps up to 1270, then 100 Mbit/s
steps.

### Choosing the ceiling

`--cir` sets the ceiling to 125% of CIR by default. On a 300/30 DOCSIS line whose real sustained rate was only about
340–345 Mbit/s, that put the ceiling (375) above what the line could actually carry: the controller parked up near
the ceiling with a standing modem queue just under the cut threshold, and rarely cut.

Measure your line's sustained loaded rate and set `--ceil` just above it, instead of trusting the 125% default. On
the test line the right ceiling turned out to be about 113–116% of CIR: `--ceil 348` (the rate itself still only
moves in 10 Mbit/s hardware steps, so it capped out at 340).

8-stream loaded download, 90 s runs, same line:

| Ceiling | % of CIR | Throughput (mean) | Latency p95 | Latency p99 (range) | Cuts per run |
|---|---|---|---|---|---|
| 375 (default `--cir 300`) | 125% | ~321 Mbit/s | ~36.6 ms | 38–61 ms | 2–5 |
| 348 | 116% | 306 Mbit/s | 30.6 ms | 32–36 ms | 0 |
| 330 | 110% | 295 Mbit/s | 29.4 ms | 31–37 ms | 0–1 |

Single-stream downloads were too noisy in this test to tell the three ceilings apart; the 8-stream numbers above are
the ones that separate cleanly.

Override the ceiling alongside `--cir` with an explicit `--ceil`:

```
option args '--cir 300 --ceil 348'
```

(`--ceil` overrides only the ceiling; floor and start still come from `--cir`.)

## Building

GitHub Actions builds both packages with `openwrt/gh-action-sdk` and the 25.12.6 `mediatek-filogic` SDK image, and runs
the host self-tests (`kmod/qdma-rl/src/enc_test.c`, `controller/qdma-autorate/src/ctl_test.c`).

With a local SDK, add this repository as a feed (`src-link qdma /path/to/repo` in `feeds.conf`), then:

```sh
./scripts/feeds update qdma && ./scripts/feeds install -p qdma qdma-rl qdma-autorate
make package/qdma-rl/compile package/qdma-autorate/compile V=s
```

The BPF object needs the SDK's LLVM BPF toolchain. The controller is linked statically against the SDK's libbpf, libelf and
zlib.

## Disclaimer

This is experimental. It comes with no warranty. The kernel module writes MT7986 hardware registers directly, outside the
Ethernet driver. Keep a backup of your router (a config backup at least, and ideally a way to reflash) before you install
it.

License: GPL-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE) for the vendored ePPing code and the MediaTek register
definitions.
