#!/usr/bin/env python3
"""Summarise an accbench run: python3 report.py [RESULTS_DIR]

Default RESULTS_DIR is /var/tmp/accbench/results/latest. Writes report.md
there and prints it. Every figure is a median over the run's repetitions;
the spread column shows how much the repetitions disagreed.
"""

import csv
import math
import os
import re
import statistics
import sys
from collections import defaultdict

REF = "c-gcc"
IMPLS = ["c-gcc", "c-clang", "rust"]
METHOD_ORDER = ["vfs", "handle", "bulkstat", "e2fs", "raw", "rawsort"]


def load(path):
    groups = defaultdict(list)
    with open(path) as f:
        for row in csv.DictReader(f):
            key = (row["fs"], row["set"], row["regime"], row["impl"], row["method"])
            groups[key].append(row)
    return groups


def med(rows, field):
    vals = [float(r[field]) for r in rows if r.get(field) not in (None, "", "<not counted>")]
    return statistics.median(vals) if vals else math.nan


def spread(rows, field):
    vals = [float(r[field]) for r in rows if r.get(field)]
    if len(vals) < 2 or statistics.median(vals) == 0:
        return math.nan
    return (max(vals) - min(vals)) / statistics.median(vals)


def secs(ns):
    return f"{ns / 1e9:.2f}" if not math.isnan(ns) else "-"


def order(ms):
    return sorted(ms, key=lambda m: METHOD_ORDER.index(m) if m in METHOD_ORDER else 99)


def geomean(xs):
    xs = [x for x in xs if x > 0 and not math.isnan(x)]
    return math.exp(sum(map(math.log, xs)) / len(xs)) if xs else math.nan


def main():
    rdir = sys.argv[1] if len(sys.argv) > 1 else "/var/tmp/accbench/results/latest"
    g = load(os.path.join(rdir, "results.csv"))
    out = []
    p = out.append

    env = open(os.path.join(rdir, "env.txt")).read() if os.path.exists(os.path.join(rdir, "env.txt")) else ""
    p("# accbench results\n")
    p("```\n" + env.strip() + "\n```\n")
    img = os.path.join(rdir, "images.txt")
    if os.path.exists(img):
        p("<details><summary>Images (generator output, free space, fragmentation, verified counts)</summary>\n")
        p("```\n" + open(img).read().strip() + "\n```\n</details>\n")

    fss = sorted({k[0] for k in g})
    regimes = sorted({k[2] for k in g}, key=lambda r: ["cold", "cold-hash", "warm", "warm-hash"].index(r)
                     if r in ["cold", "cold-hash", "warm", "warm-hash"] else 9)

    # 1. methods, reference implementation
    p(f"## 1. Access methods ({REF}; seconds, median)\n")
    p("Each cell: **total** (find / read). *find* is everything except reading file content: walking, "
      "metadata, opening, extent mapping. *read* is the content reads (plus hashing in -hash regimes). "
      "`req` is the mean request size the loop device received, in KiB.\n")
    for fs in fss:
        methods = order({k[4] for k in g if k[0] == fs})
        for regime in regimes:
            sets = sorted({k[1] for k in g if k[0] == fs and k[2] == regime})
            if not sets:
                continue
            p(f"### {fs}, {regime}\n")
            p("| set | " + " | ".join(methods) + " |")
            p("|---|" + "---|" * len(methods))
            for s in sets:
                cells = []
                for m in methods:
                    rows = g.get((fs, s, regime, REF, m), [])
                    if not rows:
                        cells.append("-")
                        continue
                    t, fi, rd = med(rows, "total_ns"), med(rows, "find_ns"), med(rows, "read_ns")
                    reads, sect = med(rows, "dev_reads"), med(rows, "dev_sectors")
                    req = f", req {sect * 512 / reads / 1024:.0f}" if reads and reads > 0 else ""
                    cells.append(f"**{secs(t)}** ({secs(fi)} / {secs(rd)}{req})")
                p(f"| {s} | " + " | ".join(cells) + " |")
            p("")

    # 2. languages
    p("## 2. Rust vs C (ratio to c-gcc; below 1.00 is faster)\n")
    p("Geometric mean over all sets. *wall* is total time; *cyc u* and *ins u* are user-space CPU cycles and "
      "instructions retired (perf), which isolate the programs' own code from the kernel and the device. "
      "*spread* is the median (max-min)/median across repetitions of the reference: ratios smaller than it are noise.\n")
    for fs in fss:
        methods = order({k[4] for k in g if k[0] == fs})
        p(f"### {fs}\n")
        p("| regime | method | spread | c-clang wall | rust wall | c-clang cyc u | rust cyc u | c-clang ins u | rust ins u |")
        p("|---|---|---|---|---|---|---|---|---|")
        for regime in regimes:
            for m in methods:
                sets = sorted({k[1] for k in g if k[0] == fs and k[2] == regime and k[4] == m})
                if not sets:
                    continue
                ratios = defaultdict(list)
                spreads = []
                for s in sets:
                    ref = g.get((fs, s, regime, REF, m), [])
                    spreads.append(spread(ref, "total_ns"))
                    for impl in ("c-clang", "rust"):
                        rows = g.get((fs, s, regime, impl, m), [])
                        for f in ("total_ns", "cycles_u", "instr_u"):
                            a, b = med(rows, f), med(ref, f)
                            if b and not math.isnan(a) and not math.isnan(b):
                                ratios[(impl, f)].append(a / b)
                sp = statistics.median([x for x in spreads if not math.isnan(x)] or [math.nan])
                cell = lambda i, f: f"{geomean(ratios[(i, f)]):.2f}" if ratios[(i, f)] else "-"
                p(f"| {regime} | {m} | {sp:.0%} | {cell('c-clang', 'total_ns')} | {cell('rust', 'total_ns')} | "
                  f"{cell('c-clang', 'cycles_u')} | {cell('rust', 'cycles_u')} | "
                  f"{cell('c-clang', 'instr_u')} | {cell('rust', 'instr_u')} |")
        p("")

    # 3. syscalls
    tdir = os.path.join(rdir, "trace")
    if os.path.isdir(tdir):
        p("## 3. System calls (strace -c, warm)\n")
        p("| trace | calls | top calls |")
        p("|---|---|---|")
        for fn in sorted(os.listdir(tdir)):
            text = open(os.path.join(tdir, fn)).read()
            rows = re.findall(r"^\s*[\d.]+\s+[\d.]+\s+\d+\s+(\d+)\s+(?:\d+\s+)?(\w+)\s*$", text, re.M)
            total = next((int(t) for t in re.findall(r"^\s*100\.00\s+[\d.]+\s+\d+\s+(\d+)", text, re.M)), None)
            top = sorted(((int(c), n) for c, n in rows if n != "total"), reverse=True)[:5]
            p(f"| {fn[:-4]} | {total if total is not None else '-'} | " + ", ".join(f"{n} {c}" for c, n in top) + " |")
        p("")

    text = "\n".join(out)
    with open(os.path.join(rdir, "report.md"), "w") as f:
        f.write(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
