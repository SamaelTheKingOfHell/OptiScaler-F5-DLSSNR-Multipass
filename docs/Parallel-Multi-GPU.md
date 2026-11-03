# Parallel multi-GPU prototype

Status: experimental implementation; game retest and throughput measurement pending.

The objective is higher completed-picture throughput with both GPUs available.
Pass placement is a scheduling choice, not a fixed user requirement. The first
prototype overlaps frames while retaining the current pass placement controls.
Changing dependent passes within one frame does not make those passes concurrent.

```text
Game GPU:       render N + prefix N       render N+1 + prefix N+1
Secondary GPU:             suffix N                    suffix N+1
Presentation:                         completed N                    completed N+1
```

The presentation GPU displays an entire completed picture, including its own
matching original colour, guides and post-processing. The pipeline adds roughly
one picture of latency. It must not apply an old neural answer to a new picture.
The queue is bounded: one NR recording may be suspended globally, while the game
renders ahead. A separate copy presenter retains its source and backbuffer until
its own retirement fence completes.

```text
capture owned picture N -> record prefix -> signal outbound -> suspend NR recording
render game N+1 -> resume/compose N -> capture N+1 -> submit NR N+1 -> present N
```

The existing NR renderer owns shared exposure, histories, descriptors and scratch
resources. This first prototype overlaps game rendering with secondary inference;
primary NR prefix recording waits until the previous picture's composition is
recorded. A cooperative coroutine retains the recording's local variables and
settings. It releases NR's mutex and compute-state envelope while suspended, and
must finish before another NR dispatch or resource invalidation. Capture-only
game hooks may run during that interval. View snapshots are not full NR snapshots.

```text
ready -> outbound submitted -> pending worker -> returned -> composition submitted
      -> retirement tracked -> reusable on completed fence or proven same queue order
```

Queue-ordered buffer reuse requires the exact same real primary queue: the old
inbound read and its successful retirement signal precede the next outbound write
and ready signal. Legacy callers retain the completed-fence reuse gate. No GPU
queue waits on a fence controlled by a worker that may exit.

The prototype is optional and initially limited to ordinary native DirectX 12
Finished Picture without frame generation, residual placement, multiview, Superior
Scaling, spatial packing or detail/cache reuse, and to at most three passes.
Unsupported paths retain the
existing behavior. Resize, mode changes and shutdown must drain or quarantine the
pending recording before releasing or reusing its resources.

```ini
[DlssNr]
MultiGpu=true
MultiGpuPipelined=true
```

Validation must distinguish functional return, actual overlapping GPU work,
throughput, end-to-end latency and visual/temporal quality. A lower remaining CPU
wait does not establish faster neural inference or higher game FPS. Real model
tests, process failure tests, GPU debug checks and an in-game retest are required.

```text
Verified: 30 real-model synchronous/asynchronous transport outputs matched exactly.
Verified: gated same-queue reuse preserved the earlier returned picture.
Verified: worker exit and invalid-guide rejection left the primary queue usable.
Verified: WARP/debug presenter copied entire completed pictures exactly, rejected
          incompatible resources, handled a full ring and drained finite waits.
Pending: complete in-game path, delivered FPS, input latency and visual quality.
```
