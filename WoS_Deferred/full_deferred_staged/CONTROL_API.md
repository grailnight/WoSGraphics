# Renderer diagnostic control

Include `deferred_control.h` after the actual `renderer_probe.h` and
`deferred_runtime.h`. This header owns no persistent D3D objects and does not
change rendering state.

Integration hooks:

- `WosDeferredControl::BeforePresent(device)`: after all frame drawing and
  immediately before the original native Present. Marks the GPU profiler frame
  boundary, then executes a pending screenshot.
- `WosDeferredControl::AfterPresent(device, result)`: after native Present, after
  the existing RSM callback. Polls commands at most every 300 ms. It also invokes
  the runtime's existing throttled `WriteStatus()` on successful Presents so
  `observed_present_fps` remains current while deferred is disabled.
- `WosDeferredControl::Reset()`: before device Reset. Cancels a queued capture
  with an acknowledgement; retains request deduplication.

Write the fixed file `WoS_Deferred/full_deferred/control.ini` atomically:

```ini
[Control]
RequestId=1
Enabled=1
View=0
Intensity=1
Specular=1
Capture=1
Trace=1
ReloadMaterials=0
IndirectEnabled=0
GpuProfiler=0
GpuProfilerReset=0
EligibilityCache=1
ContactEnabled=0
ContactLength=0.5
ContactThickness=0.5
ContactSteps=16
ContactStrength=1
ContactBias=0.05
```

`RequestId` is required: positive decimal uint64, strictly increasing during
the DLL's process lifetime. All remaining keys are optional. Enabled, Capture
Trace, IndirectEnabled, ReloadMaterials, GpuProfiler, GpuProfilerReset and
EligibilityCache and ContactEnabled accept 0 or 1; View accepts 0..4; Intensity accepts finite 0..3;
ContactLength accepts finite 0.05..10, ContactThickness 0.01..10, ContactSteps an
integer 2..32, ContactStrength and ContactBias 0..1;
Specular accepts finite 0..2. Unknown keys, duplicate keys, malformed numbers,
NUL bytes, other sections and files larger than 4096 bytes are rejected.
Comments occupy their own `;` or `#` line. UTF-8 BOM and CRLF are accepted.
No command accepts a destination path, executable, shell text or URL.

A new valid request after successful Present applies its settings together and
calls the existing `SaveSettings()` once if any settings were supplied.
Capture queues one readback of the next final backbuffer. With the game's
CPU-ahead rendering that frame can predate settings sent in the same request;
send the settings first and capture with a following request when the image
must reflect them. Trace calls the existing `WosProbe::Begin`
after Present and acknowledges that capture started; the usual probe lifecycle
finishes its trace at the following Present. A busy existing probe is reported
without replacing it. A failed Present rejects the request; retry with a new ID.

Acknowledgements are atomically replaced at
`WoS_Deferred/full_deferred/control_status.json`. Fields include `request_id`,
`applied`, `capture_pending`, `trace_started`, `phase`, `screenshot`,
`trace_folder`, `reload_requested`, `reloaded`, `indirect_enabled`,
`gpu_profiler`, `eligibility_cache`, `contact_enabled`, `contact_length`, `error` and current settings. `applied` describes in-memory
settings/command acceptance; `SaveSettings()` retains its existing void API.
Wait for the matching ID and `capture_pending=false`; inspect `error`.

BMP files use only the fixed `full_deferred/diagnostics` directory and generated
`capture_<process-id>_<request-id>.bmp` names. Readback uses GetBackBuffer,
CreateOffscreenPlainSurface in system memory and GetRenderTargetData. Integer
RGB/BGR 24/32-bit, R5G6B5 and X1/A1R5G5B5 are supported; files are 24-bit,
top-down BMP with padded rows. MSAA or unsupported formats receive explicit
errors. No resolve, texture binding or render-state change is performed.
Capture requires synchronous GPU readback and can stall the requested frame.

Run the isolated tests from the game directory:

```powershell
C:\Python314\python.exe WoS_Deferred\full_deferred_staged\tests\run_control_checks.py
```

The native HAL smoke passes 74 parser cases, eight source-format cases
and 402 assertions; fourteen isolated CLI protocol checks also pass. A 7x5 next-frame backbuffer pattern is captured while a
different surface is bound as RT0; all compared rendering state remains exact.
Settings/probe dispatch uses small stubs here, while frozen production API
header hashes and the complete DLL build verify the real integration.
Evidence is in `tests/control_receipt.json` and `tests/control_gpu_test.log`.

## Transactional material reload

Include `deferred_material_reload.h` **inside** `WosDeferred`, immediately after
`Load`, and assign `WosDeferredControl::reloadMaterials = WosDeferred::ReloadMaterials`
when installing the hooks. `ReloadMaterials=0` or an omitted key is inert.
The CLI switch is `control.py --reload-materials`.

A reload is accepted only after successful Present, with no pending batch,
no active runtime operation and no unsafe D3D state. The runtime checks these
conditions again and requires the same initialized device and resolve shader.
A rejected reload does not apply settings, capture or trace from that request.
The request ID is consumed; use a new ID to retry.

All input/primary pairs are read from the fixed installed `materials` directory.
Files remain open without write/delete sharing until validation finishes.
Every existing supported native input must remain represented, no duplicate
native inputs are allowed, and limits are 512 variants, 1 MiB per shader and
64 MiB total shader input. Input CTAB and shader token boundaries are checked;
all replacement shaders must be created successfully by the actual device.
Optional `<base>.policy.ini` is bounded to 4096 bytes and accepts only
`[Material] PointCountMultiple=1` or `2` (default1).

Only after all checks succeed does the new vector replace the old material and
guard vector. Old cached native shader references are released and guards are
rebuilt on demand. Settings, enabled/fault state, resolve shader, render targets,
camera and frame counters remain intact. Any failed preparation retains the old
pack and cache. The controller reports `reloaded=true` only on success.

Publish the complete GPU-verified pack before sending the command. This API
validates loading and ownership; it does not establish shader semantic equivalence
or verify an external proof receipt. Isolated HAL transaction evidence is in
`tests/material_reload_receipt.json`; run `tests/run_material_reload_checks.py`.

## GI enable control

`IndirectEnabled=0|1` invokes the existing GI reset path through
`WosDeferredGIControl::SetEnabled` after successful Present. The installed hook
assigns `WosDeferredControl::setIndirectEnabled` to that callback. The CLI switch
is `--indirect-enabled 0|1`. Bounce, ambient, radius and distance are preserved.
Repeated requests for the current enabled state do not repeat the GI reset.

A request combining an actual material reload and IndirectEnabled is rejected
before mutation. The CLI requires these operations to be separate requests.
Pending/busy/unsafe runtime state or an unavailable callback also rejects the
switch. A successful acknowledgement includes the current `indirect_enabled`.

## GPU profiler

`GpuProfiler=0|1` switches the optional timestamp profiler in
`deferred_gpu_profiler.h`; `GpuProfilerReset=1` clears its statistics window
without changing the switch. The CLI flags are `--gpu-profiler 0|1` and
`--gpu-profiler-reset`. The state is stored as `[Profiler] Gpu` in settings.ini
and is also a checkbox in the in-game menu. Both keys are accepted while a batch
is pending and combine freely with reload and GI requests, because the profiler
issues queries only and owns no rendering decision.

`BeforePresent` opens one disjoint window per frame with a frame-begin stamp and
closes the previous one. Sections are bracketed inside the existing work:
`primary_batch` from the opening of a primary batch to the entry of its resolve,
`hdr_copy_in` and `hdr_copy_out` around the two `StretchRect` copies of the
resolve, `resolve_quad` around the fullscreen resolve, and `gi_merge` around the
GI receiver merge. Primary geometry is thousands of draws per frame, so it is
not stamped per draw: `primary_batch` is a span that also contains any native
draws interleaved in that batch, and its `calls` equals the number of resolve
batches. Results are read from an older ring slot with `GetData(...,0)`, so no
frame waits for its own results and no flush is forced. Four frames are in
flight, 128 stamps are available per frame with the last reserved for the frame
end, and 256 completed frames form the statistics window. A frame that runs out
of stamps is still reported and counted in `truncated_frames`.

`full_deferred_status.json` carries a `gpu_profiler` object with `enabled`,
`supported`, `status`, `window_frames`, `completed_frames`, `discarded_frames`,
`skipped_frames`, `truncated_frames`, `frame_span_ms` and per-section
mean/median/p95 plus the average number of calls per frame. The status revision
is now 3.

`frame_span_ms` measures one frame boundary to the next and therefore includes
Present, vsync and idle time; the sections do not sum to it and it must not be
compared with the CPU submission timers. A disjoint window, a lost query or an
incomplete result increments `discarded_frames` instead of producing a duration.
A device without timestamp queries sets `supported=false` and keeps rendering
unchanged. Screenshots and traces stall the frame they belong to, so they are
not mixed with a measurement window.

Evidence is in `tests/gpu_profiler_receipt.json`; run
`tests/run_gpu_profiler_checks.py`, which drives a real HAL device through 40
measured frames and 646 assertions, including the open batch interval and its
cross-frame guard.

## Eligibility state cache

`EligibilityCache=0|1` sets the user request for the state shadow in
`deferred_state_shadow.h`; the CLI flag is `--eligibility-cache 0|1`, the
setting is `[Cache] Eligibility`, and the menu has a checkbox. The shadow
answers the per-draw eligibility reads (render states, RT0..RT3, depth surface
and its container, viewport, pixel shader) from copies kept current by the
setter hooks: SetRenderState, SetViewport, SetRenderTarget,
SetDepthStencilSurface, SetPixelShader, plus the runtime's own tracked shader
setter. State block Apply, EndStateBlock and device Reset invalidate every
entry; a recording state block suspends updates. An entry that is not known
is fetched with the real Get, so the shadow only removes calls, never changes
an answer. Nothing is remembered until the integration reports all hooks
installed, which keeps isolated tests on the real device.

Every 64th check verifies each known entry against the device. One mismatch
disables the shadow for the session, counts in `mismatches`, and leaves the
user request untouched. `full_deferred_status.json` carries
`eligibility_cache` with `requested`, `enabled`, `status`, per-frame `hits`
and `fills`, and cumulative `verifications`, `mismatches`, `invalidations`.
Evidence is in `tests/state_shadow_receipt.json`; run
`tests/run_state_shadow_checks.py`.

## Contact shadows

`ContactEnabled`, `ContactLength`, `ContactThickness`, `ContactSteps`,
`ContactStrength` and `ContactBias` set the screen-space contact shadow of the
resolve; the CLI flags are `--contact-*`, the setting section is `[Contact]`,
and the menu has the same controls. The resolve marches up to `Steps` depth
samples from the surface toward the sun over `Length` world units, compares
depth in linear view units against `Thickness`. Each step contributes
`entering * leaving * (1 - t / Length)`, ramping in past the depth bias and out
at the thickness limit, and the march keeps the largest contribution rather than
returning on the first hit, so occluders far from the receiver darken less and
the far end of the shadow fades. The contribution is continuous on purpose: a
first-hit boolean let the per-pixel jitter decide the whole result, and its
fixed per-pixel phase advance printed a comb of teeth along every boundary.
Every pixel now runs the same step count, so the loop does not diverge. The term is gated by
`smoothstep(.35, .9, nativeVisibility)`: pixels inside the native penumbra keep
their own softness and are not marched. The result is combined with the native
sun visibility by `min`, never multiplied. Length 0 (or
Enabled 0) leaves the shader on its original arithmetic. View 4 shows the
contact mask alone. The runtime derives the camera-relative forward matrix by
inverting the camera inverse in double; a failed inversion skips contact for
that batch and counts in `contact.matrix_failures`.
