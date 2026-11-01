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

On 2026-11-01 the NVIDIA RTX 3060 plus WARP fixture passed with the D3D12 debug layer enabled. The RTX 5070 Ti enumerated but `D3D12CreateDevice` returned `0x8007000e`, so no two-physical-GPU result was obtained. Retry the default test once both adapters can create D3D12 devices.

## Ordered submission prototype

`CrossAdapterSubmit` validates command-list and queue ownership by adapter LUID and enqueues three already-closed, caller-owned lists in primary, secondary, primary order. A pure sequencing test injects failures into each fence signal or wait and checks that the protocol refuses further use after a partial submission. The coordinator has no way to split a game command list that is still being recorded; it is not integrated into the neural path. The caller remains responsible for resource states, feature lifetime, submission retirement and handling device loss.

A separate diagnostic on 2026-11-01 confirmed that the RTX 5070 Ti owns the primary display and that both D3D11 and D3D12 device creation on it returned `0x8007000e`; the RTX 3060 successfully initialized both APIs. Windows commit headroom was low, but the cause of the allocation failure is not yet confirmed.

## Frame transport prototype

`CrossAdapterFrame` allocates color and motion transports together with optional depth. Their copies can share a single outbound fence; only edited color needs to return. A controlled RTX 3060/WARP fixture transferred three differently sized, padded textures in one submission sequence and verified all three inputs on the secondary plus the returned color on the primary. It also checked that an unsupported third channel leaves no partially published frame and that depth may be absent. No neural evaluation was performed.

## Mixed-generation model handling

The current installation guide distinguishes the RTX 50 runtime from a cross-generation runtime for RTX 20/30/40. A multi-GPU session must probe the selected model separately on each device and preserve each pass feature and its capability parameters with its own device. Do not infer that a DLL working on the game GPU also works on the co-processor, or substitute a runtime without an explicit user choice. Where a compatible runtime supports both devices, verify that actual pair before reusing it. A failure on either adapter must leave the existing single-device path available before any cross-device work is submitted.
