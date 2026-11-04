# Copyright (c) 2026 ByteDance
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Combine memcpy measurements and export a plot, CSV, and aligned table."""

import argparse
import csv
import json
from pathlib import Path
from statistics import geometric_mean

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("results", type=Path, nargs="+")
parser.add_argument("--out", type=Path, required=True)
parser.add_argument(
    "--three-way",
    action="store_true",
    help="Plot only scalar, the fastest fixed RVV kernel, and Zimemops",
)
parser.add_argument(
    "--lmul",
    action="store_true",
    help="Also plot m1/m2/m4/m8 RVV kernels, with and without alignment",
)
parser.add_argument(
    "--rvv-kernel",
    help="Fixed RVV kernel for --three-way (default: fastest over all cases)",
)
parser.add_argument(
    "--labels", nargs="+", help="One Zimemops series label per results file"
)
args = parser.parse_args()
if args.three_way and len(args.results) != 1:
    parser.error("--three-way accepts one implementation's results file")
if (args.lmul or args.rvv_kernel) and not args.three_way:
    parser.error("--lmul and --rvv-kernel require --three-way")
if args.labels and len(args.labels) != len(args.results):
    parser.error("--labels must provide one label per results file")
args.out.mkdir(parents=True, exist_ok=True)
rows = []
for index, path in enumerate(args.results):
    for row in json.loads(path.read_text()):
        row.setdefault("unroll", 1)
        if row.get("memory") != "ddr5-6400":
            parser.error("DDR5-6400 results are required")
        row["implementation"] = (
            "rvv-m8" if row["method"] == "rvv" else row["method"]
        )
        if row["method"] == "zimemops":
            engine = "stream" if row.get("streaming", False) else "reference"
            row["implementation"] = (
                f"zimemops-{engine}-b{row.get('transfer_bytes', 1)}"
                f"-u{row['unroll']}"
            )
            row["implementation"] += f"-scalar{row.get('scalar_threshold', 0)}"
            if row.get("quad_batch", False):
                row["implementation"] += "-quad"
            elif row.get("pair_control", False):
                row["implementation"] += "-pair"
            if args.labels:
                row["implementation"] = f"zimemops-{args.labels[index]}"
        rows.append(row)
rows.sort(
    key=lambda r: (r["mode"], r["offset"], r["size"], r["implementation"])
)
keys = [
    "implementation",
    "memory",
    "memory_channels",
    "memory_bus_bits",
    "dram_tck_ps",
    "dram_trcd_ps",
    "mode",
    "size",
    "offset",
    "reps",
    "unroll",
    "window_bytes",
    "vlen",
    "buffer_slots",
    "slot_bytes",
    "peak_read_bytes",
    "peak_write_bytes",
    "buffer_peak_bytes",
    "buffer_stalls",
    "streaming",
    "scalar_threshold",
    "quad_batch",
    "pair_control",
    "store_priority",
    "store_reserve",
    "coalesce",
    "prefetch",
    "cycles",
    "cycles_per_copy",
    "bytes_per_cycle",
    "sim_insts",
    "sim_ops",
    "branch_mispredicts",
    "store_reserve_stalls",
    "l2_read_ex_requests",
    "squashed_loads",
]
with (args.out / "results.csv").open("w", newline="") as output:
    writer = csv.DictWriter(
        output, fieldnames=keys, extrasaction="ignore", lineterminator="\n"
    )
    writer.writeheader()
    writer.writerows(rows)
(args.out / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
best = {}
rvv_cases = {}
for row in rows:
    if row["implementation"].startswith("rvv-"):
        key = (row["mode"], row["size"], row["offset"])
        rvv_cases.setdefault(key, {})[row["implementation"]] = row
        if key not in best or row["cycles"] < best[key]["cycles"]:
            best[key] = row
plot_rows = rows + [{**r, "implementation": "rvv-best"} for r in best.values()]
comparisons = {}
fixed_comparisons = {}
for row in rows:
    if row["method"] != "zimemops" or not row["size"]:
        continue
    key = (row["mode"], row["size"], row["offset"])
    if key not in best:
        continue
    comparisons.setdefault(row["implementation"], []).append(
        best[key]["cycles_per_copy"] / row["cycles_per_copy"]
    )
    fixed = fixed_comparisons.setdefault(row["implementation"], {})
    for variant, rvv in rvv_cases[key].items():
        fixed.setdefault(variant, []).append(
            rvv["cycles_per_copy"] / row["cycles_per_copy"]
        )
summary = {}
for name, ratios in comparisons.items():
    fixed = fixed_comparisons[name]
    fastest = min(fixed, key=lambda variant: geometric_mean(fixed[variant]))
    summary[name] = {
        "geomean_speedup_vs_best_rvv": geometric_mean(ratios),
        "wins": sum(r > 1 for r in ratios),
        "nonzero_cases": len(ratios),
        "best_fixed_rvv": fastest,
        "geomean_speedup_vs_best_fixed_rvv": geometric_mean(fixed[fastest]),
        "wins_vs_best_fixed_rvv": sum(r > 1 for r in fixed[fastest]),
        "min_speedup_vs_best_rvv": min(ratios),
        "max_speedup_vs_best_rvv": max(ratios),
        "fixed_rvv": {
            variant: {
                "geomean_speedup": geometric_mean(values),
                "wins": sum(r > 1 for r in values),
                "cases": len(values),
                "min_speedup": min(values),
            }
            for variant, values in fixed.items()
        },
    }
(args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
series = sorted(
    {r["implementation"] for r in plot_rows},
    key=lambda label: (
        (2, label)
        if label.startswith("zimemops")
        else (0 if label == "scalar" else 1, label)
    ),
)
if args.three_way:
    if len(summary) != 1:
        parser.error("--three-way requires one Zimemops configuration")
    zimemops = next(iter(summary))
    rvv = args.rvv_kernel or summary[zimemops]["best_fixed_rvv"]
    if rvv not in fixed_comparisons[zimemops]:
        parser.error(f"missing RVV kernel: {rvv}")
    rvv_label = f"RVV ({rvv[4:]})"
    labels = {"scalar": "scalar", rvv: rvv_label, zimemops: "Zimemops"}
    plot_rows = [
        {**row, "implementation": labels[row["implementation"]]}
        for row in rows
        if row["implementation"] in labels
    ]
    if not any(row["implementation"] == "scalar" for row in plot_rows):
        parser.error("--three-way requires scalar results")
    series = ["scalar", rvv_label, "Zimemops"]
    coverage = {}
    for label in series:
        data = [r for r in plot_rows if r["implementation"] == label]
        cases = {(r["mode"], r["offset"], r["size"]) for r in data}
        if len(cases) != len(data):
            parser.error(f"duplicate cases for {label}")
        coverage[label] = cases
    if any(cases != coverage["scalar"] for cases in coverage.values()):
        parser.error("all three implementations must cover the same cases")

    case_comparisons = []
    for row in rows:
        if row["implementation"] != zimemops or not row["size"]:
            continue
        key = (row["mode"], row["size"], row["offset"])
        reference = rvv_cases[key][rvv]
        case_comparisons.append(
            {
                "mode": row["mode"],
                "size": row["size"],
                "offset": row["offset"],
                "zimemops_cycles": row["cycles_per_copy"],
                "fixed_rvv": rvv,
                "fixed_rvv_cycles": reference["cycles_per_copy"],
                "speedup_vs_fixed_rvv": reference["cycles_per_copy"]
                / row["cycles_per_copy"],
                "best_rvv": best[key]["implementation"],
                "best_rvv_cycles": best[key]["cycles_per_copy"],
                "speedup_vs_best_rvv": best[key]["cycles_per_copy"]
                / row["cycles_per_copy"],
            }
        )
    (args.out / "case-comparisons.json").write_text(
        json.dumps(case_comparisons, indent=2) + "\n"
    )

modes = sorted({r["mode"] for r in rows})
memories = {r["memory"] for r in rows}
if len(memories) != 1:
    parser.error("plot each memory configuration separately")
memory = next(iter(memories)).upper()
offsets = sorted({r["offset"] for r in rows})


def draw(data_rows, labels, filename, metric, max_size=None):
    fig, axes = plt.subplots(
        len(modes),
        len(offsets),
        squeeze=False,
        figsize=(4.8 * len(offsets), 3.8 * len(modes)),
        layout="constrained",
    )
    palette = ["#777777", "#0072B2", "#009E73", "#CC79A7", "#56B4E9"]
    colors = (
        {
            label: (
                "#D55E00"
                if label.lower().startswith("zimemops")
                else palette[index % len(palette)]
            )
            for index, label in enumerate(labels)
        }
        if len(labels) <= 6
        else {}
    )
    for i, mode in enumerate(modes):
        for j, offset in enumerate(offsets):
            ax = axes[i, j]
            for label in labels:
                data = sorted(
                    (
                        r
                        for r in data_rows
                        if r["mode"] == mode
                        and r["offset"] == offset
                        and r["size"] > 0
                        and (max_size is None or r["size"] <= max_size)
                        and r["implementation"] == label
                    ),
                    key=lambda r: r["size"],
                )
                ax.plot(
                    [r["size"] for r in data],
                    [
                        (
                            r[metric]
                            if metric != "cycles_per_byte"
                            else r["cycles_per_copy"] / r["size"]
                        )
                        for r in data
                    ],
                    marker="o",
                    markersize=3.5,
                    linewidth=1.4,
                    color=colors.get(label),
                    label=label,
                )
            ax.set_xscale("log", base=2)
            ax.set_yscale("log")
            ax.set_xlabel("Bytes per copy")
            ax.set_ylabel(
                {
                    "bytes_per_cycle": "Bytes/cycle (higher is better)",
                    "cycles_per_copy": "Cycles/copy (lower is better)",
                    "cycles_per_byte": "Cycles/byte (lower is better)",
                }[metric]
            )
            alignment = (
                "destination - source" if mode == "overlap" else "offset"
            )
            ax.set_title(f"{mode}, {alignment} {offset} B")
            ax.grid(True, which="both", alpha=0.25)
            ax.tick_params(labelsize=8)
    handles, legend_labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(
        handles,
        legend_labels,
        loc="outside lower center",
        ncol=min(6, len(labels)),
        fontsize=9,
    )
    fig.suptitle(
        f"gem5 RISC-V O3 memcpy: {memory}, VLEN={rows[0]['vlen']}\n"
        "2 GHz; read/write each 32 B/cycle; shared LSU banks 128 B"
    )
    for suffix in ("png", "svg"):
        fig.savefig(args.out / f"{filename}.{suffix}", dpi=180)
    plt.close(fig)


draw(
    plot_rows,
    series,
    "comparison",
    "bytes_per_cycle" if args.three_way else "cycles_per_byte",
)
if args.three_way:
    draw(plot_rows, series, "comparison-latency", "cycles_per_copy")
    draw(plot_rows, series, "comparison-short", "cycles_per_copy", 256)
table_sets = [(plot_rows, series, "tables.md")]
if args.lmul:
    for aligned in (False, True):
        prefix = "rvv-align-" if aligned else "rvv-"
        variants = [prefix + f"m{lmul}" for lmul in (1, 2, 4, 8)]
        lmul_labels = {
            "scalar": "scalar",
            zimemops: "Zimemops",
            **{variant: f"RVV ({variant[4:]})" for variant in variants},
        }
        lmul_rows = [
            {**r, "implementation": lmul_labels[r["implementation"]]}
            for r in rows
            if r["implementation"] in lmul_labels
        ]
        for variant in variants:
            cases = {
                (r["mode"], r["offset"], r["size"])
                for r in lmul_rows
                if r["implementation"] == lmul_labels[variant]
            }
            if cases != coverage["scalar"]:
                parser.error(f"incomplete LMUL coverage for {variant}")
        lmul_series = (
            ["scalar"]
            + [lmul_labels[variant] for variant in variants]
            + ["Zimemops"]
        )
        name = "comparison-lmul" + ("-aligned" if aligned else "")
        draw(lmul_rows, lmul_series, name, "bytes_per_cycle")
        draw(lmul_rows, lmul_series, name + "-latency", "cycles_per_copy")
        table_sets.append((lmul_rows, lmul_series, name + "-tables.md"))
for data_rows, labels, filename in table_sets:
    lines = []
    for mode in modes:
        position = "distance" if mode == "overlap" else "offset"
        for offset in offsets:
            lines.extend(
                [
                    f"### {mode}, {position} {offset}, cycles per operation",
                    "",
                    "| Bytes | " + " | ".join(labels) + " |",
                    "|---:|" + "---:|" * len(labels),
                ]
            )
            sizes = sorted(
                {
                    r["size"]
                    for r in rows
                    if r["mode"] == mode and r["offset"] == offset
                }
            )
            for size in sizes:
                values = []
                for label in labels:
                    row = next(
                        (
                            r
                            for r in data_rows
                            if r["mode"] == mode
                            and r["offset"] == offset
                            and r["size"] == size
                            and r["implementation"] == label
                        ),
                        None,
                    )
                    values.append(
                        f"{row['cycles_per_copy']:.2f}" if row else "-"
                    )
                lines.append(f"| {size} | " + " | ".join(values) + " |")
            lines.append("")
    (args.out / filename).write_text("\n".join(lines) + "\n")
