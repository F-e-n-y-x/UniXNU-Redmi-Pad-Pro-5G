# Task for agy: macOS on ruan without PAC (research, gather, build)

## Goal
Find out whether macOS (UniXNU macOS profile, e.g. `macos-27.3-26C5035r`, or sheng's macOS 26.5) can run on the
Redmi Pad Pro 5G "ruan". Collect everything that exists, then build the most promising path.

## The blocker
- ruan: SM7435, 4x Cortex-A78 + 4x Cortex-A55 (ARMv8.2, **no FEAT_PAuth/PAC**), Adreno 710, 8 GB, Gunyah hypervisor.
- UniXNU's macOS profiles are arm64e only and need native PAC (see UniXNU `docs/porting/arm64e.md` "Requirements"
  and `docs/porting/macos.md`). macOS system binaries are arm64e.
- So the question: can arm64e macOS kernel + userland run on a CPU without PAC?

## Research (search widely: GitHub code/issues/PRs, UniXNU forks, Telegram channels except the user's own group, forums)
1. Any project running arm64e iOS/macOS code on non-PAC ARM: PAC instructions in the HINT space (PACIASP/AUTIASP
   etc. are NOPs on v8.2 — check which ones are HINT vs which trap: PACIA/AUTIA/BLRAA/RETAA/LDRAA are NOT hints and
   are UNDEFINED on v8.2), binary rewriting, trap-and-emulate in the kernel, Corellium-style approaches, QEMU/TCG.
2. Can macOS userland be run as arm64 (non-e) slices? Which system binaries/dylibs ship arm64 slices in the macOS 26/27 IPSW?
3. Kernel side: can XNU be built `target=macosx` without PAC (`--os macos-27` with PAC disabled), as the iPadOS 17
   arm64 profile does? What breaks (ptrauth in kexts, prelinked arm64e kexts)?
4. Check UniXNU upstream (https://github.com/UniXNU/UniXNU) and forks: UniXNU-sheng, sheng-macos-link-lab (mataht),
   UniXNU-tb710fu (limingerf), vildangil/UniXNU-Dizi-Ruan (same SoC as ruan).

## Build
- Best path from the research: e.g. macOS kernel built arm64 (no PAC) + PAC-instruction emulation in the
  undefined-instruction handler, or rewriting arm64e binaries. Write findings first, then build.
- Board package to start from: `/root/unixnu-work/UniXNU-ruan` (iPadOS 17 arm64 port that already boots to SpringBoard
  with the GPU). Work on a **copy** (e.g. `/root/unixnu-work/UniXNU-ruan-macos`) and a separate UniXNU core checkout;
  a Claude agent is updating `/root/unixnu-work/UniXNU` and `UniXNU-ruan` right now, so don't edit those.
- macOS builds need a Mac with Xcode per UniXNU docs; note what can be done on Linux.

## Tablet / repo rules
- The user gave full permission for this task. Still: test boots are RAM-only `fastboot boot`
  (`/root/unixnu-work/boot-local.sh <tag>` when the tablet is on this PC); don't flash or repartition without telling
  the user; don't scan the network.
- Repo "/DATA/tower/Projects/Redmi Pad Pro 5G": commit only paths under `macOS/` (`git add macOS/...`, never `git add -A`).
  Put notes in `macOS/docs/research/macos-no-pac.md`.

## Context to read first
- `macOS/docs/IPADOS-BRINGUP.md` (current state, §24–26), `macOS/docs/research/unixnu-site-notes.md`,
  `macOS/docs/research/community/*.md`.
