# Multi-GPU multipass plan (not implemented)

The game device remains the presentation device. Each neural pass should be independently assignable to the game device or a compatible secondary device. Passes remain ordered: a pass consumes the preceding pass result, and the final image is returned to the game device before its normal composition and presentation. Multiple devices do not imply independent passes can execute concurrently.

## Required implementation

1. Identify the game adapter by its actual device LUID, enumerate compatible neural adapters, and expose explicit per-pass placement with a same-device default. Probe model, runtime, format and transport support before accepting a secondary device.
2. Give every pass a persistent feature and temporal history on its owning device. Rebuild and safely retire the affected generation on device, resolution, profile, placement or pass-count changes; propagate cuts and resets to every pass.
3. Preserve the immutable base and ordered intermediate outputs. Copy all required color and guide inputs across a real submission boundary using compatible cross-adapter shared resources and fences or a tested staging fallback. Copy the final result to the game device and synchronize before it is consumed. Restore resource states and honor ownership and lifetime on every path.
4. Account for command lists still being recorded: a secondary queue cannot consume data that the game has not submitted. Integrate at a verified submission boundary without waiting on a fence that depends on the blocked game queue. Bound failure paths and fall back safely to the original same-device path.
5. Test alternating and grouped placements, no secondary GPU, invalid adapter, feature creation failure, resizing, camera cuts, device removal, multiple viewports and frame-generation interactions. Verify pixel output and temporal behavior on actual supported games and both GPUs, measure transfer latency and confirm presentation stays on the game GPU.

The adjacent DLSS Bridge project documents a secondary-GPU transport for some capture paths, but its native D3D12 secondary path explicitly stands aside. Its root has no repository-wide license declaration at the inspected revision; do not copy code without clear permission. DirectX 11, Vulkan and D3D12 require separate integration and validation.
