# HakiOS host tools (for `unixnu-ruan/scripts/make-ramroot.py`)

`make-ramroot.py` builds the iPadOS root with these, and finds them here by default (`HAKIOS=` overrides).
Run `./build.sh` once to compile `xnu-deps/host/bin/unzbm`.

| File | From | Licence |
|---|---|---|
| `tools/apfs/mkapfs.py`, `tools/apfs/apfs.py` | HakiOS ([oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) `tools/apfs/`), with this port's changes | MIT (that repo's `tools/`) |
| `sysimage/select-system.py` | HakiOS (`darwin/tools/select-system.py` there), with this port's changes | HakiOS's licence (see its `NOTICE.md`) |
| `xnu-deps/host/src/` | `kernel/deps/build/lzbitmap/` at `62e09f8`, unchanged | GPL-2.0+ OR MIT (Corellium LLC), used under MIT |

Use these copies, not upstream's: upstream's versions differ and are not what the port was built with.
