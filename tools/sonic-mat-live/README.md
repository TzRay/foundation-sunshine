# Native sources to a dedicated MAT worker

This opt-in tool captures pre-mix object passes and hands owned PCM/XYZ/gain to
the dedicated native worker, then publishes complete IEC carrier cycles through
the private live pipe. The selected UUYC/plugin route uses
`sunshine-plugin-object-tap.dll` and `sunshine-plugin-mat-capture.exe`.
Objects remain the default source. The same publisher now accepts explicit
`manage-live.ps1 -Source bed -Endpoint <id|default>` for 48 kHz float32 stereo,
5.1 or 7.1 endpoint PCM. Bed mode submits a full static 7.1.4 bed with absent
channels silent; it installs no object-tap hooks and keeps a silent render stream
active so an idle endpoint continues producing capture packets. A manually
started bed capture uses the endpoint resolved at startup.

## Managed Sunshine integration

With the matching Sunshine build, select `mat_live = enabled` and
`mat_source = bed` or `mat_source = native`. The managed source uses `audio_sink` (name or endpoint ID);
an empty value follows the current default output. Bed capture does not select
the Opus virtual-sink policy. The interactive-user publisher selects and restores
the default output for each MAT lease.

When the selected endpoint or mix format changes, the manager retires the old
publisher. A recovery-capable client can resume audio in the current video
session. An explicit capture stop sends EOS so the client can drain buffered audio.

`native` forwards the Windows endpoint's own Dolby MAT output, preserving the
system mix, height/object audio and metadata instead of re-encoding loopback PCM.
Select an endpoint already configured for Dolby Atmos for home theater. A silent
shared-mode render client keeps that endpoint's Windows graph active; application
audio still passes through the normal Windows renderer. Two pinned callbacks
bind the copied output to the explicit endpoint and Dolby implementation.
No old startup frames or synthetic MAT are inserted. Client PLAY still selects
current complete cycles, so receiver acquisition from an ongoing stream remains
a hardware acceptance requirement.

The native hook is bundled in `sunshine-sonic-mat-worker.dll`; installation keeps
the existing three-file deployment contract. `native-output-capture.exe` also
provides bounded `--capture-endpoint` / `--publish-endpoint` diagnostics. Native
output uses actual callback completion QPC, not a calibrated original media PTS.
It rejects missing callbacks, renderer changes and invalid cycles. A device
invalidation ends the current epoch and permits the manager to retry only after
the helper has stopped and both original entries have been restored. Unsupported
formats fail; they do not silently fall back to the PCM-bed source.

Run `install-bed.ps1 -Build <native-build> -Output <fresh-backup-directory>` as
administrator. Optional `-HostPackage <Sunshine-package>` and
`-ExpectedHostSha256 <hash>` update the host and Web UI with backups and rollback;
`-AudioSink <name-or-ID>` selects the PCM endpoint. The native installer registers
the `Sunshine MAT PCM` logon task for the current user with highest privileges.
It starts after that user logs in; it is not a SYSTEM boot service, because
Dolby Access package registration belongs to the user.

The supervisor follows the host PID/creation time, enable/source flags, endpoint format
and audiodg PID/creation time. It drains its owned child before starting a new
epoch, including after a fast Sunshine restart. An enabled MAT request starts
the installed task if absent and asks the manager to retry a failed/missing
capture. A running capture is reused. Requests do not enable a disabled MAT
configuration. Host acknowledgement and source preparation share a 12-second
budget; failures still return RTSP 503. A new request, host generation or MAT
toggle clears the failure lock; it never terminates an audio engine or unloads
a blocked worker.
`sunshine-plugin-mat-capture.exe --stop-manager` requests an orderly stop.
Status and per-run logs are in `%ProgramData%\Sunshine\MAT`.

Internal worker protocol v5 retains source capture QPC separately from encoding
completion QPC. Only bed source rounds follow WASAPI directly; warm/drain and
object capture retain timer pacing. Ready requires source-stage carrier data;
warm-up alone is insufficient. These software checks do not verify AVR clock
synchronization. When host MAT is enabled, an unavailable requested MAT source
returns RTSP 503; changing an accepted MAT negotiation to another codec returns
415. Ordinary non-MAT clients retain their usual audio behavior.

Drain cycles and the pipe END record continue the source timeline. They do not
switch back to completion/wall-clock QPC, which can regress relative to the
capture timestamp of a virtual endpoint. This is timestamp bookkeeping, not a
measurement of the receiver's playback clock.

The private ABI is pinned to audioeng.dll 10.0.26100.9711, timestamp 0x0627570d,
image size 0x1ef000 and SHA256
`3936bffdc4aa365e74c9995359f2821aaf759a14155a180cfa95764962b9f1d3`; the verified
builds live only in `src/spatial_audio/mat_compat.h`.
The launcher checks the explicit PID/creation time, system/native component
identities and reviewed DLL hashes. Object mode also checks original callback
bytes; bed preflight checks the selected endpoint format without starting it. This is an opt-in
development path, not a general Windows capture API.

The object bridge accepts one renderer, 48 kHz/480-frame full-span dynamic
objects and 7.1.4 static channels, with at most 20 complete inputs in total.
Static channels keep their AudioObjectType, including LFE; no XYZ is invented.
Unsupported static types, multiple sources for one bed channel (mixing is not
implemented), fragmented objects, other rates, missing inputs and multiple active renderers fail the session. No
resampling, inferred silence, catch-up or sample-file input is provided.

A paired successful plugin Process can return buffered headphone output with no
object Submit. Such output-only drains do not create MAT object input, advance
its sequence or retire objects. Zero-output missing-object passes, partial object
loss and capture record gaps still fail; no silence or timed frame is inferred.

Build with CMake, Visual Studio C++ tools and the Windows SDK. CMake fetches
MinHook at `c3fcafdc10146beb5919319d0683e44e3c30d537`:

```powershell
./tools/sonic-mat-live/build.ps1 -Output ./build/mat-capture-new
```

The build and CI share twelve CTests: capture options, object bridge, static-bed
mapping, route ownership, component compatibility, paired callbacks, dynamic-only
and 12 static + 2 dynamic IPC, source-clock IPC, private pipe lifecycle, native
output binding/restoration, and byte-preserving native relay.
The IPC tests run 1300 rounds with a FAKE worker and check exact type, PCM, gain,
position and EOS. CI passes `-TestsOnly`; it never attaches to audiodg, loads a
vendor encoder, starts a signal source or needs administrator privileges.
`bed-probe` is an opt-in bounded native-encoder diagnostic; `bed-source-probe
--endpoint <id|default>` plays one second of quiet per-channel tones and checks
one second of idle capture. Neither runs as part of regular CTest.

Preflight and Start first print one `COMPONENT` line each for `audioeng.dll` and
Dolby Access (version, SHA256, `supported`/`missing`/`unsupported`). Builds not
listed in `src/spatial_audio/mat_compat.h` end with exit code 3 before any hook or
elevation; `manage-live.ps1` reports them and Status shows `Unsupported`. Dolby
Access is located through its package family, so Store version folders and other
install drives work as long as the DLL contents are verified.

`manage-live.ps1` defaults to read-only `Preflight`. `Start` requires the exact
engine identity, reviewed build, fresh output and an explicit `-Continuous` or
`-Seconds 1..60`. `Status` and `Stop` take that run's `capture.json`, validate
publisher/engine identity and report observed readiness or verified restoration.
Start exposes the fixed local `SunshineMatLive-active` pipe. On first use, enable
MAT in the Sunshine UI, save and apply (restart Sunshine), then run Start.
Subsequent tool starts/restarts connect without copying any
configuration. Only one capture publisher can own this endpoint at a time.

For an explicitly requested soak test, the source generator accepts
`spatial-probe --emit --long-run --seconds 1800 --endpoint <id> --objects 2 --bed-7.1.4`.
`--long-run` raises the source duration limit to 3600 seconds; the default remains
60 seconds. Pair it with a separately authorized `-Continuous` capture and stop
that capture while the source is still active, before closing the source.

The host verifies the elevated publisher, protected deployment location and
executable content hash before exchanging capture commands.
There is no UAC, endpoint change, installed-host edit, automatic restart or retry.
Preflight/Start require only `build.json` and its three runtime files: the plugin
publisher, plugin tap and native worker. The manifest contains their required
content hashes; source trees, documentation and test executables are not runtime prerequisites.
Start deploys below Program Files and rejects untrusted owners, writable ACLs
or reparse points on the deployment path/files; it never repairs an existing ACL.
Each deployed copy must match its `build.json` hash (a build file replaced after
the manifest check is removed and rejected). Start runs its elevated preflight
and passes tap/worker hashes from the verified copies and manifest only, never
from the user-writable build directory.
Native attachment requires the operator's current authorization; running software
tests does not authorize it.

`spatial-probe.exe` is a separate endpoint inventory and explicit live-generated
object source for a separately authorized test. It does not change the default
endpoint. Enable MAT in Sunshine as described in
[MAT-HOST](../../docs/MAT-HOST.md).
For an explicitly authorized generated test, `--emit --bed-7.1.4 --objects 2`
requires the full static mask before stream activation and submits a distinct
live signal to each of the 12 bed channels plus two moving objects. It does not
substitute a smaller layout or replay a file.

Historical native reports and abandoned paths remain on the
[archive branch](https://github.com/AlkaidLab/foundation-sunshine/tree/codex/mat-history-20261023).
The separate Sonic HRTF capture entry/tap and its tests were archived at
[`d006e4b`, codex/mat-sonic-archive-20261023](https://github.com/AlkaidLab/foundation-sunshine/tree/codex/mat-sonic-archive-20261023).
The current build retains the shared bridge, native session and plugin regressions.
They do not establish current game compatibility, A/V timing or HDMI/Atmos output.

The paired recorder has eight fixed encoder bindings. Distinct empty domains
can run concurrently with the selected object domain; overlapping passes or
reset/destroy during a pass on the same encoder still fail. Producers reserve bounded queue
tickets and publish a completion marker after copying; the reader never skips an
unfinished ticket. QPC ordering is checked per domain, not across independent
threads. No callback waits, allocations or silent drops are introduced.
Only one object-bearing domain is encoded. The private capture IPC is version 7;
rebuild the tap and publisher together. This does not change the MAT wire protocol.
The paired tap also preserves the native stream content type at description +12,
through input protocol v2 and object retirement, to the pinned encoder. It does
not assign a content type to loopback PCM or manufacture Atmos metadata. Content
type preservation alone does not establish receiver Atmos recognition.
On capture failure, `PLUGIN_DIAGNOSTIC` records the first failed guard and callback
counters. No logging is performed from audio callbacks.
`firstFault` is 1 for a record validation failure, 2 for a renderer binding,
3 for nested rendering, 4 for invalid render frames, 5 for an unsupported extended
submission, or 6 for an unbound callback. The three arguments provide the guard's
observed values. The original failure and stop behavior is preserved.
Assembler rejections additionally print `PLUGIN_RECORD_REJECT` with the specific
failed check and record metadata; PCM samples are not logged.
