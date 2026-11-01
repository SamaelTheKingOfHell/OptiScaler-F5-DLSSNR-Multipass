# Multi-GPU multipass plan (not implemented)

The game device remains the presentation device. Passes run in two contiguous groups: zero or more passes on the game device first, then zero or more on a compatible secondary device. The pass split is user-selectable. The secondary group consumes the last game-device pass, or the original input if the game-device group is empty. There is at most one outbound handoff and one return; passes never alternate between devices. The final image returns to the game device before composition and presentation. Passes remain sequential because each consumes the previous result.

## Required implementation

1. Identify the game adapter by its actual device LUID, enumerate compatible neural adapters, and expose the number of initial game-device passes with a same-device default. Probe model, runtime, format and transport support before accepting a secondary device.
2. Give every pass a persistent feature and temporal history on its owning device. Rebuild and safely retire the affected generation on device, resolution, profile, placement or pass-count changes; propagate cuts and resets to every pass.
3. Preserve the immutable base and ordered intermediate outputs. Copy all required color and guide inputs across a real submission boundary using compatible cross-adapter shared resources and fences or a tested staging fallback. Copy the final result to the game device and synchronize before it is consumed. Restore resource states and honor ownership and lifetime on every path.
4. Account for command lists still being recorded: a secondary queue cannot consume data that the game has not submitted. Integrate at a verified submission boundary without waiting on a fence that depends on the blocked game queue. Bound failure paths and fall back safely to the original same-device path.
5. Test every valid split, reject alternating placements, and test no secondary GPU, invalid adapter, feature creation failure, resizing, camera cuts, device removal, multiple viewports and frame-generation interactions. Verify pixel output and temporal behavior on actual supported games and both GPUs, measure transfer latency and confirm presentation stays on the game GPU.

The adjacent DLSS Bridge project documents a secondary-GPU transport for some capture paths, but its native D3D12 secondary path explicitly stands aside. Its root has no repository-wide license declaration at the inspected revision; do not copy code without clear permission. DirectX 11, Vulkan and D3D12 require separate integration and validation.

## Transport prototype status

A D3D12 shared-buffer and single-plane texture transport prototype exists, with separate outbound and return fences. It has no integration into the model pass loop or game submission path. The caller must transition resources, order queues and keep each allocation alive until all work retires. Unsupported texture descriptions or mismatched adapter footprints are rejected.

Run `./tests/run_nr_cross_adapter_tests.ps1` for a two-NVIDIA-adapter round trip, with the primary display adapter first. This test convention does not prove which adapter a particular game uses; runtime integration must use that game device’s LUID. `-Warp` instead exercises an NVIDIA adapter plus Microsoft's software adapter; it is a controlled fixture, not a two-GPU or game test. `-ArgumentsOnly` checks fail-closed validation without a second device. The fixture verifies raw bytes and padded RGBA8, RGBA16F, R32F and RG16F textures, but does not run a neural model or present a game.

On 2026-11-01 the RTX 3060 plus WARP fixture passed. After recovering system commit headroom, the RTX 5070 Ti and RTX 3060 also passed the physical-adapter fixture with the D3D12 debug layer enabled.

## Ordered submission prototype

`CrossAdapterSubmit` validates command-list and queue ownership by adapter LUID and enqueues three already-closed, caller-owned lists in primary, secondary, primary order. A pure sequencing test injects failures into each fence signal or wait and checks that the protocol refuses further use after a partial submission. The coordinator has no way to split a game command list that is still being recorded; it is not integrated into the neural path. The caller remains responsible for resource states, feature lifetime, submission retirement and handling device loss.

An earlier diagnostic showed that the RTX 5070 Ti owns the primary display but D3D11 and D3D12 device creation on it returned `0x8007000e` while commit headroom was low. Device creation succeeded after system commit headroom recovered; the observation does not establish the exact driver allocation failure mechanism.

## Frame transport prototype

`CrossAdapterFrame` allocates color and motion transports together with optional depth. Their copies can share a single outbound fence; only edited color needs to return. A controlled RTX 3060/WARP fixture transferred three differently sized, padded textures in one submission sequence and verified all three inputs on the secondary plus the returned color on the primary. It also checked that an unsupported third channel leaves no partially published frame and that depth may be absent. No neural evaluation was performed.

## Mixed-generation model handling

The current installation guide distinguishes the RTX 50 runtime from a cross-generation runtime for RTX 20/30/40. A multi-GPU session must probe the selected model separately on each device and preserve each pass feature and its capability parameters with its own device. Do not infer that a DLL working on the game GPU also works on the co-processor, or substitute a runtime without an explicit user choice. Where a compatible runtime supports both devices, verify that actual pair before reusing it. A failure on either adapter must leave the existing single-device path available before any cross-device work is submitted.

The current `NVNGXProxy::InitDx12` uses a process-wide initialized flag, and `DlssNr_Dx12.cpp` caches one capability parameter block in its global model state. Calling that initializer again with a different device does not establish a second device session. The secondary path must establish and validate its own supported initialization and capability ownership without mutating the game's active NGX state; do not assume that the forwarder's per-device snippet bookkeeping alone solves the core lifetime.

## NGX core probe

`./tests/run_nr_ngx_init_probe.ps1` runs NGX in an isolated process and application-data directory. It can optionally create and evaluate a feature with explicit forwarder and runtime paths. It refuses to invoke NGX below 8 GiB of available system commit headroom; that guard is a conservative test limit, not a measured runtime requirement. `./tests/run_nr_ngx_isolated_pair.ps1` starts two separate processes and holds both features live across a synchronization barrier before reevaluating and releasing them. Neither probe connects to a game or transfers a model result between processes.

On 2026-11-01 the RTX 5070 Ti and RTX 3060 passed the two-physical-adapter buffer and texture round-trip fixture. With the user-provided NVIDIA 310.9 runtime, each adapter separately created and evaluated a 256x144 feature twice. In one process, both features evaluated while live, but releasing the second feature caused an access violation regardless of release order; loading the model from two different file paths did not change that result. A two-process fixture held both features live simultaneously, reevaluated them, and exited both processes cleanly. This supports investigating a separate secondary worker; it does not prove game integration, visual quality, or a production-ready runtime.

## Isolated cross-process neural frame prototype

`CrossAdapterHandles` exports unnamed heap and fence handles, and `CrossAdapterPeer` opens duplicated handles on the explicitly selected secondary LUID. The isolated test child inherits only a private handshake pipe; the parent duplicates the required handles directly into that child. `./tests/run_nr_cross_process_tests.ps1` checks a real shared-buffer round trip and a simulated worker exit without placing a wait for that worker on the primary GPU queue.

`./tests/run_nr_cross_process_neural.ps1 -ForwarderDll <absolute-path> -ModelDll <absolute-path>` requires a locally provided forwarder and model DLL. It creates persistent features per pass in separate processes. Two different synthetic frames run through either one primary pass and two secondary passes, or two secondary passes without a primary evaluation (`-NoPrimaryPass`); color, depth and motion travel in one outbound shared-buffer handoff and the answer returns to a primary GPU texture. The fixture uses the existing precompiled clamp shader at the inter-pass boundaries and default feedback; other production inter-pass modes are not exercised. The zero-primary-evaluation case still initializes an unused primary feature. Each process checks the D3D12 debug layer and retires work before releasing its feature. On 2026-11-01 this passed on the RTX 5070 Ti and RTX 3060 with the locally installed 310.9 runtime. An initial fixture teardown crashed in the NGX core; releasing the feature and unloading its forwarder before destroying capability parameters and shutting down the core matched the independently working probe and the subsequent runs passed. This result is limited to those synthetic frames and does not establish production safety or visual quality.

The fixture deliberately waits on the CPU for a result or worker exit, so it demonstrates ordering and bounded failure detection rather than real-time performance. It is not connected to the application, does not implement all model settings or interpass shaders, and has not been exercised inside a game.

## Owned-list and process-exit prototypes

`OwnedCommandChain` operates only on caller-owned direct command lists from the same actual device and queue. It allocates a continuation before closing and submitting the current list, preserves its continuation allocators and inserts no GPU wait. The caller must retain the chain and initial allocator until its independently tracked retirement fence completes. The neural fixture splits the primary model and transfer lists this way; it also rejects a list from the other device. This helper does not take ownership of, split or submit a game-owned list and is not wired into the application's finished-picture path.

`WaitForFenceOrPeer` performs a bounded CPU wait, checking the peer process between fence polls. It does not leave an event registered on an unsignalled fence, and it never queues a GPU wait on a fence controlled by a process that might exit. A separate fixture checks a real child-process exit, a timeout and a completed fence. Polling adds latency and no real-time game performance is claimed.

## Standalone worker executable

The solution now builds `dlssnr_worker.exe`. It owns an isolated NGX session with one feature per secondary pass and uses the existing precompiled clamp shader between passes. The versioned private-pipe protocol carries duplicated handles, pass configuration, sequential frame commands, temporal reset bits, and shutdown. Pass tuning that differs from the built feature is rejected; the caller must rebuild the worker generation. It supports up to 30 contiguous secondary passes in the protocol, although only one through four have been tested so far.

The Release worker itself passed synthetic two-frame tests on the RTX 5070 Ti and RTX 3060, including one primary followed by four secondary passes and a zero-primary-evaluation case at 320x181 with aligned shared-buffer offsets. It still supports only RGBA16F color, R32F depth and RG16F motion at the model dimensions, and currently uses the normal clamp path at default feedback in the test. It is not started by the application. Model/runtime selection, worker generation ownership, alternate formats and guide shapes, full setting changes, game submission, fallback, game presentation and deployment remain incomplete.
