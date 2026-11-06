# Helper GPU share

Status: experimental. Results so far come from a real two-GPU fixture with
synthetic input. Throughput, seam visibility and temporal quality in a game are
not verified yet.

A second GPU helps only while its part of each picture finishes in time. If a
pass is too slow on the helper as a whole, the helper now takes only the
picture's bottom rows of that pass. The game GPU evaluates the top band of the
same pass at the same time.

```text
passes on the game GPU     passes placed on the helper
+--------------------+     +--------------------+
| whole picture      |     | top band: game GPU | rows 0 .. seam + halo
|                    |     |--------------------| <- seam
|                    |     | strip: helper GPU  | rows seam - halo .. end
+--------------------+     +--------------------+
```

## Settings

```ini
[DlssNr]
MultiGpu=true
SecondaryPassMask=4        ; pass 3 is the helper's (example)
MultiGpuPipelined=true     ; recommended: the helper's turnaround may span one frame
MultiGpuHelperShare=0      ; 0 automatic, 1..100 fixed percentage of the rows
MultiGpuQueueWait=true     ; the game GPU waits for the helper on its queue, not the CPU
```

The same controls are on the Passes page, on the card of the first helper pass.

## Automatic share

`DlssNr_HelperShare.h` decides how many rows the helper takes. For every
returned picture, a worker watcher thread records the time from the game GPU's
outbound signal to the helper's return signal. It also records the period
between consecutive outbound signals, which is the GPU frame time.

- It starts with whole passes, the earlier behavior.
- If the helper's turnaround stays above 85% of the frame period for four
  samples, it shrinks the rows in proportion. It shrinks a little more than
  that, because part of each pass costs the same however few rows the helper
  has.
- If the turnaround stays below 70% of that target for at least a second, it
  grows the rows by 10%, up to whole passes again.
- Rows stay on the model's 16-pixel grid. Each side evaluates 32 extra rows
  past the seam, so neither side's answer ends right at the seam.
- A share change moves the strip's first row. The helper's feature history
  restarts for that picture.
- The minimum is 64 rows. A helper that is late even at the minimum stays
  there, and the menu reports it as at its minimum.

## GPU-side wait and watchdog

Previously the CPU waited for the helper's result before recording the
picture's composition. With `MultiGpuQueueWait`, the game GPU's queue waits on
the helper's return fence instead, and the CPU keeps preparing frames.
`WorkerWatch` guards that wait. If the worker process exits, or the helper does
not return within 3 seconds of the outbound signal, the watcher terminates the
worker and signals the fence from the CPU. The queue continues, the generation
reports failure, and the remaining passes return to the game GPU. That picture
keeps stale helper rows.

Buffer reuse is ordered by the same queue: the next outbound copy is queued
behind the previous return copy. A new frame therefore does not wait on the CPU
for the previous one to retire.

## Transfer

In strip mode only the strip's colour rows go to the shared buffer and come
back. Depth and motion still go whole. The worker copies the strip to the top
of a working texture, evaluates that many rows, applies the inter-pass clamp to
the same rows, and writes its answer back at the strip's own rows. The game GPU
copies only the rows below the seam into its final answer.

## Fixture

`tests/nr_gpu/nr_helper_strip.cpp` runs on adapter 0 (game GPU) and adapter 1
(helper) with a synthetic picture. For each share it reports the median helper
turnaround. It checks that the returned strip rows are written and finite. It
kills the worker after the GPU wait is queued, and checks that the queue is
released and the failure is reported.
