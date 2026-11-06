"""Computes DLSS-NR split transfer sets from kernel parameters alone (no trial runs).

Reads a launches CSV written by nr_cuda_trace (raw parameter blobs) and the workspace base, and for a cut/resume pair
writes the forward and backward workspace ranges in the split-sets format used by nr_split_splice / nr_split_owner:
    F <offset> <bytes>
    B <offset> <bytes>

Decoded parameter layout (docs/NR-network-trace.md): tensor pointers first, then the weights pointer, then the padded
height and width of that stage as 32-bit fields, then pointers into the synchronization area (workspace offsets below
64 KiB). A stage tensor is H * W * C bytes (FP8) with C = 32 * 384 / H at 1280x720, i.e. channels double as the
resolution halves; in general C is taken from the kernel family and the first stage's height.

A tensor crosses the cut when the same pointer appears in a kernel at or before the cut and in a kernel of the helper's
segment (forward), or in the segment and at or after resume (backward). Its extent is the largest shape-derived size seen
for that pointer. The whole synchronization area always crosses both ways.

Usage: python nr_split_sets.py <launches.csv> <frame> <workspace base hex> <workspace bytes> <cut> <resume> <out>
       [--compare <split-sets.txt>] [--roles <roles.txt>]
"""
import csv
import struct
import sys

SYNC_BYTES = 64 * 1024


def kernels(path, frame):
    rows = [r for r in csv.DictReader(open(path)) if r['frame'] == str(frame)]
    return [(r['kernel'], bytes.fromhex(r['raw'])) for r in rows]


def uses(blob, base, size):
    """(workspace offset, H, W) for every workspace pointer: dims = the first plausible (H, W) pair after it."""
    out = []
    pointers = []
    o = 0
    while o + 8 <= len(blob):
        q = struct.unpack_from('<Q', blob, o)[0]
        if base <= q < base + size:
            pointers.append((o, q - base))
        o += 8
    dims = []
    for o in range(0, len(blob) - 7, 4):
        h, w = struct.unpack_from('<II', blob, o)
        if 8 <= h <= 4096 and 8 <= w <= 4096 and w > h and (h % 4 == 0) and (w % 4 == 0):
            dims.append((o, h, w))
    for o, off in pointers:
        after = [d for d in dims if d[0] > o]
        h, w = (after[0][1], after[0][2]) if after else (0, 0)
        out.append((off, h, w))
    return out


def channels(h, first_h):
    # 32 channels at the first Swin stage (height first_h), doubling each time the height halves.
    return 32 * first_h // h if h and first_h and h <= first_h else 0


def compute(ks, base, size, cut, resume):
    # The first Swin stage's padded height: the first dims of the first swin_1h_32 view kernel. Picture-level dims
    # (pre/post blocks carry the picture size) are larger and are ignored for tensor extents.
    first_h = 0
    for name, blob in ks:
        if 'swin_1h_32' in name and 'pre_block' not in name and 'post_block' not in name:
            for _, h, _ in uses(blob, base, size):
                if h:
                    first_h = h
                    break
        if first_h:
            break
    extent = {}
    seen = {}
    for i, (name, blob) in enumerate(ks):
        for off, h, w in uses(blob, base, size):
            if off < SYNC_BYTES:
                continue
            nbytes = h * w * channels(h, first_h) if h and h <= first_h else 0
            extent[off] = max(extent.get(off, 0), nbytes)
            seen.setdefault(off, set()).add(0 if i <= cut else 1 if i < resume else 2)
    forward = [(0, SYNC_BYTES)]
    backward = [(0, SYNC_BYTES)]
    for off, segments in sorted(seen.items()):
        n = min(extent[off] or 256, size - off)
        if 0 in segments and 1 in segments:
            forward.append((off, n))
        if 1 in segments and 2 in segments:
            backward.append((off, n))
    return forward, backward


def refine(fwd, bwd, roles_path, cut, resume):
    """Drops tensors by measured roles (roles.txt from nr_split_splice SPLICE_ROLES, read-only snapshots):
    forward: a tensor the first segment kernel referencing it overwrites (>= 95%) is output space, not input;
    backward: a tensor the segment never changed is still valid on the owner; one the first end kernel referencing it
    overwrites is output space."""
    writes, refs = {}, {}
    for line in open(roles_path):
        p = line.split()
        if p[0] == 'W':
            writes.setdefault(int(p[1]), []).append((int(p[2]), int(p[3])))
        elif p[0] == 'R':
            refs.setdefault(int(p[1]), set()).add(int(p[2]))

    def written(kernel, o, n):
        return sum(max(0, min(o + n, a + m) - max(o, a)) for a, m in writes.get(kernel, []))

    def referenced(kernel, o, n):
        return any(o <= r < o + n for r in refs.get(kernel, ()))

    def keep_forward(o, n):
        if o < SYNC_BYTES:
            return True
        for k in range(cut + 1, resume):
            if referenced(k, o, n):
                return written(k, o, n) < 0.95 * n
        return False

    def keep_backward(o, n):
        if o < SYNC_BYTES:
            return True
        if not any(written(k, o, n) for k in range(cut + 1, resume)):
            return False
        for k in sorted(refs):
            if k >= resume and referenced(k, o, n):
                return written(k, o, n) < 0.95 * n
        return False

    return [r for r in fwd if keep_forward(*r)], [r for r in bwd if keep_backward(*r)]


def liveness(ks, base, size, roles_path, cut, resume):
    """Block-level liveness through the segment. The workspace is a reused pool, so each use is taken with the extent
    of that kernel's own stage (pointer, padded H x W x C). Walking the receiving kernels in order, a referenced block
    must arrive when no earlier receiving kernel, and not the referencing kernel itself, has written it (writes measured
    by read-only snapshots, roles.txt). Backward data is limited to blocks the segment changed."""
    block = 256
    first_h = first_w = 0
    for name, blob in ks:
        if 'swin_1h_32' in name and 'pre_block' not in name and 'post_block' not in name:
            hw = [(h, w) for _, h, w in uses(blob, base, size) if h]
            if hw:
                first_h, first_w = hw[0]
                break
    starts = sorted({off for _, blob in ks for off, _, _ in uses(blob, base, size)})

    def gap(off):
        later = [s for s in starts if s > off]
        return min(later[0] - off if later else 256, 1 << 20)
    writes = {}
    for line in open(roles_path):
        p = line.split()
        if p[0] == 'W':
            writes.setdefault(int(p[1]), set()).update(range(int(p[2]) // block, (int(p[2]) + int(p[3])) // block))

    def regions(kernel):
        out = set()
        for off, h, w in uses(ks[kernel][1], base, size):
            if off < SYNC_BYTES:
                continue
            if h and h <= first_h:
                n = h * w * channels(h, first_h)
            elif h:  # picture-level dims (pre/post blocks): the tensor is a first-stage one
                n = first_h * first_w * 32
            else:  # no dims after the pointer: up to the next known tensor
                n = gap(off)
            out.update(range(off // block, min(off + n, size) // block))
        return out

    def live(first, last, eligible=None):
        written, needed = set(), set()
        for k in range(first, last):
            refs = regions(k) - written - writes.get(k, set())
            needed |= refs if eligible is None else refs & eligible
            written |= writes.get(k, set())
        return needed

    segment_writes = set().union(*(writes.get(k, set()) for k in range(cut + 1, resume)))
    fwd = live(cut + 1, resume)
    bwd = live(resume, len(ks), segment_writes)

    def ranges(blocks):
        out, b = [(0, SYNC_BYTES)], sorted(blocks)
        i = 0
        while i < len(b):
            j = i
            while j + 1 < len(b) and b[j + 1] == b[j] + 1:
                j += 1
            out.append((b[i] * block, (j - i + 1) * block))
            i = j + 1
        return out

    return ranges(fwd), ranges(bwd)


def merge(ranges):
    out = []
    for o, n in sorted(ranges):
        o0, o1 = o & ~255, (o + n + 255) & ~255
        if out and o0 <= out[-1][1]:
            out[-1][1] = max(out[-1][1], o1)
        else:
            out.append([o0, o1])
    return [(a, b - a) for a, b in out]


def covered(ranges, truth):
    have = 0
    for o, n in truth:
        for a, m in ranges:
            lo, hi = max(o, a), min(o + n, a + m)
            if hi > lo:
                have += hi - lo
    return have


def main():
    path, frame, base, size, cut, resume, out = sys.argv[1:8]
    base, size, cut, resume = int(base, 16), int(size), int(cut), int(resume)
    ks = kernels(path, frame)
    fwd, bwd = compute(ks, base, size, cut, resume)
    if '--roles' in sys.argv:
        fwd, bwd = liveness(ks, base, size, sys.argv[sys.argv.index('--roles') + 1], cut, resume)
    fwd, bwd = merge(fwd), merge(bwd)
    with open(out, 'w') as f:
        for o, n in fwd:
            f.write(f"F {o} {n}\n")
        for o, n in bwd:
            f.write(f"B {o} {n}\n")
    mib = lambda rs: sum(n for _, n in rs) / 1048576
    print(f"cut {cut}, resume {resume}: forward {mib(fwd):.2f} MiB in {len(fwd)} ranges, "
          f"backward {mib(bwd):.2f} MiB in {len(bwd)} ranges")
    if '--compare' in sys.argv:
        truth_f, truth_b = [], []
        for line in open(sys.argv[sys.argv.index('--compare') + 1]):
            k, o, n = line.split()
            (truth_f if k == 'F' else truth_b).append((int(o), int(n)))
        for label, mine, truth in (('forward', fwd, truth_f), ('backward', bwd, truth_b)):
            c = covered(mine, truth)
            total = sum(n for _, n in truth)
            print(f"  {label}: covers {c / 1048576:.2f} of {total / 1048576:.2f} MiB measured as needed "
                  f"({100.0 * c / max(total, 1):.1f}%)")


if __name__ == '__main__':
    main()
