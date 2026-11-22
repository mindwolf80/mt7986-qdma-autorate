# mt7986-qdma-autorate

Hardware download shaping for the GL.iNet GL-MT6000 (Flint 2, MediaTek MT7986) on OpenWrt 25.12.6.

## What it is

I moved download bufferbloat control on my Flint 2 router into the network chip's own hardware. The router measures
latency passively from normal traffic, with no pings or scripts, and a small controller adjusts the chip's built-in speed
limiter on the fly. On my 300 megabit cable line it gets about 335 megabits, where CAKE gets about 285, with the same low
latency. It uses almost no CPU, starts at boot, and recovers on its own from crashes.

The catch: a single big download on its own is still slower than with CAKE. My theory is that the controller is too
aggressive with single connections.

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
  round-trip time passively from normal traffic, including from pure ACKs.
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

- **Single downloads:** a single bulk download gets about 140 Mbit/s, against about 244 with CAKE. The likely cause is that
  the controller cuts too often on a lone flow.
- **Idle:** at idle the rate sits at its 150 Mbit/s floor and ramps up within a few seconds once traffic starts.
- **UDP/QUIC:** UDP/QUIC traffic gives no RTT samples. Only TCP flows drive the controller.
- **Testing:** tested on one router and one ISP only.
- **No XDP:** native XDP on the WAN interface (`eth1`) resets the link on this hardware, so this project uses tcx.

## Settings

Everything is in `/etc/config/qdma-autorate`. The controller flags are in `option args`:

```
config qdma-autorate 'main'
	option enabled '1'
	option wan 'wan'
	option lan_dev 'eth0'
	option queues '3,4,5,6,8'        # QDMA queues of the LAN ports (DSA port + 3)
	option watchdog_ms '2000'
	option args '--start 300 --floor 150 --ceil 380 --cut-thresh 10 --inc-thresh 5 --dec 0.95 --cut-cooldown-ms 400 --bind-backlog-frac 0.8 --probe-step 10 --probe-interval-ms 500 --fast-step 20'
```

Restart after a change: `service qdma-autorate restart`.

| Flag | Meaning |
|---|---|
| `--start` | rate in Mbit/s at start |
| `--floor` | lowest rate the controller will set |
| `--ceil` | highest rate (a safety bound; the rate only rises while the LAN queue is backlogged) |
| `--cut-thresh` | cut the rate when the median per-flow delay exceeds this many ms |
| `--inc-thresh` | only raise the rate while the delay is below this many ms |
| `--dec` | factor for a cut (0.95 = −5%) |
| `--probe-step`, `--probe-interval-ms` | slow increase: this many Mbit/s at most once per interval |
| `--fast-step` | increase per 100 ms tick when recovering after a cut |
| `--csv PATH` | write a per-tick CSV for debugging (off by default) |

`qdma-autorate -h` lists every flag.

**Example for a gigabit line** (not tested by me): set the floor to about half the line rate, and the ceiling a bit
above the speed you actually get:

```
	option args '--start 800 --floor 400 --ceil 1000 --cut-thresh 10 --inc-thresh 5'
```

The hardware encodes rates as MAN × 10^EXP kbit/s: 1 Mbit/s steps up to 127, 10 Mbit/s steps up to 1270, then 100 Mbit/s
steps.

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
