# Pixel Fold Field Runtime

**Status:** bootstrap milestone; device verification pending  
**Target:** Google Pixel Fold running DSCO from `~/dsco/dsco-cli`  
**Entrypoint:** `dsco-device`

## Field contract

The phone is not a shrunken desktop. It is a private, embodied DSCO node whose
interface changes with the active window. The first deployable lane uses the
official Termux environment because the repository is a POSIX C CLI and no
Android application exists yet.

### Acceptance criteria

1. Build a native `aarch64` DSCO binary on the device.
2. Launch from any directory through `dsco-device`.
3. Recompute the viewport on every launch and expose compact, medium, or expanded
   layout state without assuming a specific hinge posture.
4. Keep DSCO state private under `~/.dsco`.
5. Default to the lite startup profile and deny network, subprocess execution,
   secrets, and control-plane capabilities until deliberately granted.
6. Pass `--version` and the governed `cwd` tool smoke test.
7. Remain useful without cloud connectivity when a local provider is configured.

## Install

Install the current official Termux build from its supported distribution
channel, not an obsolete Play Store package. In Termux:

```bash
pkg update
pkg install git
mkdir -p ~/dsco
git clone https://github.com/arthurcolle/dsco.git ~/dsco/dsco-cli
cd ~/dsco/dsco-cli
bash scripts/pixel-fold/install-termux
```

Launch and diagnose:

```bash
dsco-device
bash ~/dsco/dsco-cli/scripts/pixel-fold/doctor
```

The installer compiles from source on the Pixel. It does not copy the macOS
binary: that artifact is Mach-O and cannot run on Android.

## Capability posture

`dsco-device` starts with:

```text
DSCO_ALLOW_READ=1
DSCO_ALLOW_WRITE=1
DSCO_ALLOW_NET=0
DSCO_ALLOW_RUN=0
DSCO_ALLOW_SECRETS=0
DSCO_ALLOW_CONTROL=0
```

Grant only what a session needs:

```bash
DSCO_ALLOW_NET=1 dsco-device "research this topic"
DSCO_ALLOW_RUN=1 dsco-device "build and test this checkout"
```

Do not persist provider keys in shell history. Use DSCO's saved environment or
Android-backed secret integration when that lane ships.

## Fold-aware behavior

A terminal process cannot directly consume Jetpack WindowManager hinge events.
It can, however, correctly adapt to the actual window dimensions. The launcher
classifies the live viewport:

| Class | Columns | Intended presentation |
|---|---:|---|
| compact | `<80` | single-pane, terse chrome |
| medium | `80–119` | normal terminal composition |
| expanded | `>=120` | room for split inspector/timeline surfaces |

State is atomically written to `~/.dsco/device/pixel-fold.env` and exported as
`DSCO_DEVICE_LAYOUT`. DSCO's TUI must continue handling `SIGWINCH`; posture is
not equivalent to physical display size, especially in split-screen or
multi-window operation.

## Local intelligence lane

The field target is **local-first**, not necessarily **phone-only inference**.
Route in this order:

1. on-device OpenAI-compatible endpoint when thermals, memory, and model quality
   meet the task;
2. a private LAN DSCO/model node over an explicitly granted network capability;
3. a cloud provider only when explicitly selected and authorized.

The exact on-device model is deliberately not hard-coded. It must be chosen from
measured Pixel memory pressure, prompt-processing rate, generation rate,
thermal throttling, battery drain, tool-call accuracy, and offline success.

## Perfection roadmap

### M0 — reproducible terminal node (this change)

- install, build, launcher, doctor, viewport contract, conservative grants;
- static shell tests on macOS/Linux;
- device smoke test remains mandatory.

### M1 — native Android shell

Create a separate Android module rather than growing `main.c` or `tools.c`:

- Kotlin/Compose adaptive shell using window size classes;
- Jetpack WindowManager posture and separating-fold events;
- terminal/PTY bridge to the governed DSCO runtime;
- foreground-service lifecycle for bounded long operations;
- Android Keystore/BiometricPrompt credential broker;
- share-sheet, microphone, camera, notification, and haptic adapters as explicit
  capabilities, never ambient permissions.

### M2 — field-grade local intelligence

- benchmark candidate quantizations on the actual Pixel Fold;
- add thermal, battery, memory, and connectivity policy;
- resumable sessions and command idempotency across Activity/process death;
- offline eval suite for tool selection, latency, hallucination, and recovery;
- signed update manifest and rollback to a last-known-good binary/model pair.

### M3 — thousand-dimensional embodied interface

Treat HCI dimensions as sensed/control variables, not twelve labels: viewport,
posture, touch, stylus, speech, gaze proxies, motion, location, connectivity,
battery, thermal state, privacy context, trust, authority, uncertainty, mission
state, and temporal continuity. Compress these into a small number of legible
operator signals while retaining provenance and override.

## Device verification record

Run and preserve:

```bash
cd ~/dsco/dsco-cli
git rev-parse HEAD
uname -a
getprop ro.product.model
getprop ro.build.version.sdk
file ./dsco
./dsco --version
./dsco --tool-exec cwd '{}'
bash scripts/pixel-fold/doctor
```

Completion of M0 requires this evidence from the actual Pixel Fold. The current
host has no `adb` or connected Android device, so this document does not claim
physical-device validation.

## References

- Android adaptive apps: <https://developer.android.com/develop/adaptive-apps/guides/get-started-with-adaptive-apps>
- Android fold awareness / Jetpack WindowManager: <https://developer.android.com/develop/adaptive-apps/guides/foldables/make-your-app-fold-aware>
- Android NDK concepts: <https://developer.android.com/ndk/guides/concepts>
- Official Termux repository and installation notes: <https://github.com/termux/termux-app>
