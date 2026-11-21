# UniXNU vs ruan: architecture and build (2026-11-21)

From a read-only code-explorer pass. U = /root/unixnu-work/UniXNU, R = macOS/unixnu-ruan.

## Upstream pipeline

- **CLI** (U/scripts/unixnu/cli.py):
  - commands: `defconfig fetch tree build bootimg vendor_boot ipsw status clean release`;
  - `build` steps: kernel, package, firmware, payload, prelink, plus the opt-in userdata and runtime;
  - `release` writes package.manifest.json and checks the 1000-byte command line.
- **Config layers** (profiles.py), merged in this order: arch profile, soc.toml, OS profile, board.toml. Lists replace rather than merge.
- **Patch series order:** `arch: soc: os: device:`.
- **XNU:**
  - pinned in feeds.toml / unixnu.lock (commits only);
  - patched with `git apply` into tree/.cache/xnu-work;
  - built with build-xnu-4k-baseline.sh;
  - prelinked onto Apple's kernelcache (XnuPrelink) into a single Mach-O.
- **Payload FAT** (payload.py):
  - contents: unixnu.toml, kernel.macho, ADT, trustcache, optional data seed, gpuaddon, vroot;
  - the command line is capped at 1000 bytes.
- **Boot chain:**
  1. ABL loads BootShim, the UEFI FD and the DTB.
  2. UniXNU.efi reads the payload.
  3. DarwinBoot loads everything into one memory window and jumps to XNU.
  - Packed Data seed format: `DBRDISK1`.
  - Upstream has **no USB receiver**; that is a ruan addition.
- **Trust:** bundle-ipsw-trust.py plus trust-append-module.py (cdhash modules), `--owner-trust-cache`.
- **Root:** `unixnu build userdata` copies Apple's OS and SystemOS disk images unchanged into SHNGROOT windows. There is no "make root" and no trimming.
- **Data:**
  - built from a seed by runtime.py;
  - `data-persist=1|2` plus `log-reserve` keeps a persistent store at the end of the partition;
  - the store's identity is the seed's content hash.
- **GPU addon** (make-gpu-addon.py): plugin, daemon, prime list, pipeline pack and cache export go in through the payload into /private/var/ngpu.
- **Device records:** porting.toml per area, plus porting-record.py check.

## ruan's actual flow

- **Kernel:** built from a generated UFS variant board (make-ufs-board.sh string-edits board.toml).
- **Root:**
  - a hand-built trimmed APFS root made with HakiOS's select-system.py and mkapfs (monkey-patched);
  - written into an SHNGROOT image by make-ufs-root.py, a copy of userdata.py logic;
  - flashed to sda22.
- **Data:** sent over a USB receiver bolted onto DarwinBoot.
- **Boot image:** built by mkxnuimg-ruan.py (XNUP), not `unixnu bootimg`.

## Fragile points

1. **Dirty core fork:** the core and its submodules (Adreno740, AdrenoMetal, DarwinBoot) carry uncommitted edits (R/patches/unixnu/core-local-8542980.patch). A submodule update or checkout can silently wipe them.
2. **HakiOS dependency:** make-ramroot.py needs /root/hakios-garnet (tools/apfs, sysimage/select-system.py, xnu-deps unzbm) and monkey-patches module attributes.
3. **Prebuilt firmware:** SILICIUM_UEFI-log.fd is used as-is, and it can drift from the DarwinBoot source.
4. **make-ufs-board.sh** string-edits board.toml with exact-match asserts. These break whenever board.toml values change, e.g. root_match_volume_uuid.
5. **Release flow:** `unixnu release` can't run (no [bootimg] or firmware section), and R/porting.toml still says "planned" everywhere.
6. **soc.series:8** escapes the profile root (`arch:../../../patches/...`).

## Upstream features ruan doesn't use

- **data-persist plus a packed seed:** Data survives reboots and nothing goes over USB.
- **Persistent log region with a reader:** logs survive silent resets.
- **`--gpu-addon`, `--pipeline-pack`, `--prime-list`:** the GPU stack ships without changing the root.
- **`unixnu release`, `bootimg`, `vendor_boot`, `userdata`:** the rest of the standard build.
- **QEMU smoke test:** checks payload and menu changes without a tablet cycle.
- **Device-record tooling.**

## Ranked recommendations

1. Make the core and submodules reproducible: commit the local edits to branches, and record HEADs and the FD sha256 per build.
2. Fix make-ufs-board.sh to read the UUID with tomllib instead of exact-match string edits.
3. Turn on data-persist=2 (see ../opt/data-persist-howto.md).
4. Add the persistent log in the sda22 tail.
5. Decide the root policy: the stock userdata root plus the GPU addon (dizi's route), or vendor and pin the HakiOS tools.
6. Try `unixnu bootimg` in dizi's shape to drop XNUP and mkxnuimg.
7. Make `release` runnable, and move secrets and paths out of tools/cycle.sh.
8. Update R/porting.toml.
9. Fix soc.series:8.
10. Add a QEMU smoke test.
11. Keep command-line headroom (now about 987 of 1000 bytes).
