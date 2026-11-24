# Workaround for the missing PAC: running arm64e macOS on SM7435

> **Update 2026-11-13 (see [SCENE.md](scene-2026-11-13.md)):** this design already exists in two places.
> - [BxiiDN/macOS-Pad-6](https://github.com/BxiiDN/macOS-Pad-6) builds it for SM8250, which has the same instruction-set class as SM7435. It has `pacstrip.py` (static rewrite), the FGE trap emulator, a null-PAC XNU patch and real instruction counts for macOS 26.5. It doesn't boot yet.
> - Bergamota's Renegade already ran a rewritten arm64e iOS 18 userland to SpringBoard on SDM845, which has no PAC.
>
> - **[mateusseiboth/darwin-lisa](https://github.com/mateusseiboth/darwin-lisa) (MIT) has it WORKING on a Cortex-A78/A55 (SM7325): arm64e iOS 26 standalone with a GPU UI.** It reports the runtime rewrite of the cache corrupted about 900 data words per sub-cache, so the cache is rewritten offline, touching only instruction sections and re-hashing the CodeDirectory. The main executable and dyld are rewritten at page-in. Out-of-range LDRAA/LDRAB and FJCVTZS are emulated in `handle_uncategorized`.
>
> The plan for ruan is to reuse that work, not to start over.

Date: 2026-11-13. Background: [RESEARCH.md](feasibility-research.md). In short, UniXNU boots macOS natively on Qualcomm tablets, but only on CPUs with pointer authentication (PAC). SM7435 has no PAC.

## What root on Android can and cannot do

Root gives control of Android, but PAC is a feature of the CPU silicon (ARMv8.3 FEAT_PAuth) that the Cortex-A78 and A55 in this chip were built without. No root, kernel, firmware or hypervisor setting can switch it on. Once macOS boots, Android and its root are gone anyway. So the workaround has to be done in software, inside the macOS image and the XNU kernel.

## Measured on the tablet (all 8 cores, `probe/isaprobe.c`)

| Instruction | Feature | Used by macOS for | Result on SM7435 |
|---|---|---|---|
| PACIASP / AUTIASP (hint forms) | PAuth | function prologue/epilogue | **runs as NOP** (harmless) |
| PACIA, AUTIA, XPACI | PAuth | signing/authing pointers (isa, vtables, function pointers) | **SIGILL** |
| PACGA | PAuth | generic hashes | **SIGILL** |
| LDRAA | PAuth | authenticated loads (vtables) | **SIGILL** |
| RETAB, BRAA, BLRAA… (not executed; same encoding class as LDRAA) | PAuth | almost every function return and indirect call | **SIGILL** (by spec) |
| FJCVTZS | v8.3 JSCVT | JavaScriptCore | **SIGILL** |
| FCMLA | v8.3 FCMA | Accelerate (complex math) | **SIGILL** |
| LDAPR | v8.3 LRCPC | atomics | OK |
| LDAPUR | v8.4 LRCPC2 | C++ atomics with an offset (clang emits it for `-mcpu=apple-m1`) | **SIGILL** |
| CFINV / AXFLAG | v8.4/8.5 FlagM | rare | **SIGILL** |
| FRINT32Z | v8.5 | rounding | **SIGILL** |
| SB | v8.5 | speculation barrier | **SIGILL** |
| CASAL | v8.1 LSE | atomics | OK |

So PAC is not the only gap. macOS is compiled for Apple M1 (about ARMv8.5), and this chip is ARMv8.2.

## The workaround: "de-PAC" the image plus kernel emulation

Two layers, both needed:

### 1. Static rewrite of every arm64e Mach-O (offline, on the PC)

Rewrite the PAC instructions one-for-one into their non-PAC forms. Keys never exist on this CPU, so pointers stay raw and every "auth" step becomes a no-op:

| From | To |
|---|---|
| RETAA/RETAB | RET |
| BRAA/BRAB/BRAAZ/BRABZ Xn | BR Xn |
| BLRAA/BLRAB/BLRAAZ/BLRABZ Xn | BLR Xn |
| ERETAA/ERETAB | ERET (kernelcache only) |
| PACIA/PACIB/PACDA/PACDB/PACIZA…/AUTIA…/AUTIZA…/XPACI/XPACD | NOP |
| PACGA Xd,Xn,Xm | EOR Xd,Xn,Xm (only compared against itself in the same boot) |
| LDRAA/LDRAB Xt,[Xn,#off] | LDR Xt,[Xn,#off] when the offset fits (positive, a multiple of 8); otherwise leave it to layer 2 |

Hint forms (PACIBSP, AUTIBSP, XPACLRI) already run as NOPs and stay as they are.

Targets: the dyld shared cache, the kernelcache and its kexts, the cryptex libraries, `/System` binaries and dyld itself. Every rewritten file gets a new cdhash, so the trust caches have to be regenerated. UniXNU already builds custom trust caches (the owner trust bundle and VerifiedRoot tooling) and unseals the system volume (macOS-uke does this).

### 2. XNU undefined-instruction emulator (safety net)

A patch to UniXNU's ArmCpu exception path: on an undefined-instruction trap at EL0/EL1, decode and emulate these, then skip the instruction:
- any PAC instruction the rewrite missed, including JavaScriptCore's JIT code, which is generated at run time;
- FJCVTZS, FCMLA, LDAPUR, CFINV/AXFLAG, FRINT32/64, SB.

The emulator has to stay a safety net and not the main path. A trap costs about 1–3 µs, and macOS executes RETAB on nearly every function return, so with traps alone the system would crawl. Without the static rewrite it is unusable; with it, only rare instructions trap.

The kernel also has to accept arm64e executables on a non-PAC CPU and must not program PAC keys (patch the UniXNU arm64e profile's `native-pac-keys.patch` path).

## Risks and unknowns

- **Disabling the JIT** (`JSC_useJIT=false`) may be needed if JIT code traps too often, which makes Safari slow.
- **LDAPUR** might be common enough in hot C++ code that trapping it is too slow. Then the rewrite needs branch islands; whether that's needed is unverified until a real dyld cache is scanned.
- **Missed PAC instructions:** any check that relies on PAC failing (for example in security code) becomes a no-op. That's acceptable for a hobby port and unsafe for real use.
- **Licence:** Apple's licence allows macOS only on Apple hardware.

## Prerequisites that don't depend on PAC (the real long pole)

1. **UniXNU must boot on SM7435 at all.** The dizi port (vildangil/UniXNU-Dizi-Ruan) is stuck at XNU's MMU enable, running on one core. Storage, display and touch drivers don't exist yet.
2. **GPU:** there's no Adreno 710 driver. macOS Recovery has reached a software-rendered first frame on another port (elden). Whether WindowServer in full macOS runs without Metal is unverified.
3. **A Mac with Xcode** for builds (macOS SDK), or the GitHub macOS runner workflow in the UniXNU repos.

## Suggested order

1. Help the dizi/ruan iPadOS 17 port boot further: it's the same kernel bring-up macOS needs, and arm64 needs no rewrite.
2. Meanwhile, measure: download the macOS 27.3 dyld shared cache and count the PAC and v8.3+ instructions per type, which decides whether the static rewrite alone is enough.
3. Write the rewriter and test it on a single arm64e command-line tool under the iPadOS port's kernel.
4. Only then build a full macOS image for ruan.
