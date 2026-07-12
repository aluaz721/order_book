#!/usr/bin/env python3
"""
scripts/plot_benchmarks.py

Reads the JSON produced by bench_order_book and writes one PNG per figure
into docs/figures/.  Run after the benchmark:

    ./build/benchmarks/bench_order_book              \\
        --benchmark_repetitions=20                   \\
        --benchmark_report_aggregates_only=false      \\
        --benchmark_format=json                       \\
        --benchmark_out=docs/bench_raw.json

    python scripts/plot_benchmarks2.py docs/bench_raw.json

Figures produced:
    latency_summary.png        — median/p95/p99/max bar chart for every benchmark
    latency_histogram_*.png    — per-benchmark latency histogram
    sweep_scaling.png          — market order latency vs levels swept
    throughput.png             — orders/sec bar chart
    mixed_distribution.png     — latency distribution for the headline workload
    fok_ioc_comparison.png     — FOK fillable vs cancelled vs IOC
    snapshot_scaling.png       — snapshot latency vs depth
    memory_estimate.png        — per-order memory breakdown (static analysis)
    stats_table.md             — markdown table suitable for pasting into README
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

import matplotlib
matplotlib.use("Agg")                  # no display needed

import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np

# ── Style constants ────────────────────────────────────────────────────────────

BG       = "#0d1117"      # GitHub dark background
PANEL    = "#161b22"
BORDER   = "#30363d"
TEXT     = "#e6edf3"
TEXT_DIM = "#8b949e"
GREEN    = "#3fb950"
RED      = "#f85149"
AMBER    = "#d29922"
BLUE     = "#58a6ff"
PURPLE   = "#bc8cff"
TEAL     = "#39d353"
ORANGE   = "#f0883e"

SIDE_COLORS = [BLUE, GREEN, AMBER, RED, PURPLE, TEAL, ORANGE]

FIGSIZE_WIDE   = (12, 5)
FIGSIZE_SQUARE = (7, 6)
FIGSIZE_TALL   = (10, 7)
DPI            = 150

def _style() -> None:
    plt.rcParams.update({
        "figure.facecolor":  BG,
        "axes.facecolor":    PANEL,
        "axes.edgecolor":    BORDER,
        "axes.labelcolor":   TEXT,
        "axes.titlecolor":   TEXT,
        "axes.titlesize":    13,
        "axes.labelsize":    11,
        "xtick.color":       TEXT_DIM,
        "ytick.color":       TEXT_DIM,
        "xtick.labelsize":   9,
        "ytick.labelsize":   9,
        "grid.color":        BORDER,
        "grid.linewidth":    0.5,
        "legend.facecolor":  PANEL,
        "legend.edgecolor":  BORDER,
        "legend.labelcolor": TEXT,
        "legend.fontsize":   9,
        "text.color":        TEXT,
        "font.family":       "monospace",
        "lines.linewidth":   1.8,
        "patch.linewidth":   0.5,
    })

def _save(fig: plt.Figure, path: Path) -> None:
    fig.savefig(path, dpi=DPI, bbox_inches="tight", facecolor=BG)
    plt.close(fig)
    print(f"  wrote {path}")

# ── Data loading ───────────────────────────────────────────────────────────────

def load(json_path: Path) -> tuple[dict[str, list[float]], dict[str, float]]:
    """
    Returns a pair of:
      - a dict mapping benchmark name → list of real_time (ns) samples
      - a dict mapping benchmark name → items_per_second from the JSON
    Aggregate rows (mean, median, etc.) are excluded so we work with raw samples.
    """
    raw = json.loads(json_path.read_text())
    data: dict[str, list[float]] = defaultdict(list)
    meta: dict[str, float] = {}
    for bm in raw.get("benchmarks", []):
        if bm.get("run_type") == "aggregate":
            continue
        name = bm["name"].split("/repeats:", 1)[0]
        data[name].append(float(bm["real_time"]))
        if "items_per_second" in bm:
            meta[name] = float(bm["items_per_second"])

    return dict(data), meta

def stats(samples: list[float]) -> dict[str, float]:
    a = np.array(samples)
    return {
        "n":      len(a),
        "mean":   float(np.mean(a)),
        "median": float(np.median(a)),
        "p95":    float(np.percentile(a, 95)),
        "p99":    float(np.percentile(a, 99)),
        "max":    float(np.max(a)),
        "min":    float(np.min(a)),
        "std":    float(np.std(a)),
    }


def format_tput(value: float) -> str:
    if value >= 1e6:
        return f"{value/1e6:.1f}M ops/s"
    if value >= 1e3:
        return f"{value/1e3:.0f}K ops/s"
    return f"{value:.0f} ops/s"


def extract_mixed_workload_size(name: str) -> int | None:
    if not name.startswith("BM_MixedWorkload/"):
        return None
    parts = name.split("/")
    if len(parts) < 2:
        return None
    try:
        return int(parts[1])
    except ValueError:
        return None


def choose_mixed_workload_key(data: dict[str, list[float]], preferred_size: int = 100_000) -> str | None:
    preferred = f"BM_MixedWorkload/{preferred_size}"
    if preferred in data:
        return preferred
    mixed_keys = [name for name in data if name.startswith("BM_MixedWorkload/")]
    if not mixed_keys:
        return None
    return max(mixed_keys, key=lambda key: extract_mixed_workload_size(key) or 0)


def display_label(name: str) -> str:
    if name in DISPLAY:
        return DISPLAY[name]
    if name.startswith("BM_MixedWorkload/"):
        size = extract_mixed_workload_size(name)
        if size is not None:
            return f"Mixed workload\n({size//1000}k resting)"
    if name.startswith("BM_Snapshot/"):
        depth = name.split("/", 1)[1]
        return f"Snapshot\n(depth {depth})"
    if name.startswith("BM_AddMarketOrder_SweepN/"):
        levels = name.split("/", 1)[1]
        return f"Market\n(sweep {levels})"
    return name

# Friendly display names
DISPLAY = {
    "BM_AddLimitOrder_NoMatch":    "Limit add\n(no match)",
    "BM_AddLimitOrder_FullMatch":  "Limit add\n(full match)",
    "BM_AddMarketOrder_SweepN/1":  "Market\n(sweep 1)",
    "BM_AddMarketOrder_SweepN/4":  "Market\n(sweep 4)",
    "BM_AddMarketOrder_SweepN/8":  "Market\n(sweep 8)",
    "BM_AddMarketOrder_SweepN/16": "Market\n(sweep 16)",
    "BM_Cancel":                   "Cancel",
    "BM_AddIOC":                   "IOC",
    "BM_AddFOK_Fillable":          "FOK\n(fillable)",
    "BM_AddFOK_Cancelled":         "FOK\n(cancelled)",
    "BM_BestBidAsk":               "best_bid\n/ask query",
    "BM_Snapshot/5":               "Snapshot\n(depth 5)",
    "BM_Snapshot/10":              "Snapshot\n(depth 10)",
    "BM_Snapshot/20":              "Snapshot\n(depth 20)",
    "BM_MixedWorkload":            "Mixed\nworkload",
    "BM_HighContentionMatch":      "High\ncontention",
}

# ── Figure 1: Latency summary (median / p95 / p99 / max) ─────────────────────

def plot_latency_summary(data: dict[str, list[float]], out: Path) -> None:
    names = [n for n in DISPLAY if n in data]
    if not names:
        print("  [skip] no matching benchmarks for latency_summary")
        return

    s = {n: stats(data[n]) for n in names}
    labels  = [DISPLAY[n] for n in names]
    medians = [s[n]["median"] for n in names]
    p95s    = [s[n]["p95"]    for n in names]
    p99s    = [s[n]["p99"]    for n in names]
    maxs    = [s[n]["max"]    for n in names]

    x = np.arange(len(names))
    w = 0.2

    fig, ax = plt.subplots(figsize=FIGSIZE_WIDE)
    ax.bar(x - 1.5*w, medians, w, label="Median", color=GREEN,  alpha=0.9)
    ax.bar(x - 0.5*w, p95s,    w, label="p95",    color=BLUE,   alpha=0.9)
    ax.bar(x + 0.5*w, p99s,    w, label="p99",    color=AMBER,  alpha=0.9)
    ax.bar(x + 1.5*w, maxs,    w, label="Max",    color=RED,    alpha=0.7)

    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=8)
    ax.set_ylabel("Latency (ns)")
    ax.set_title("Operation Latency — Median / p95 / p99 / Max")
    ax.legend()
    ax.yaxis.grid(True, alpha=0.4)
    ax.set_axisbelow(True)

    # Annotate medians
    for xi, val in zip(x, medians):
        ax.text(xi - 1.5*w, val + max(maxs)*0.01, f"{val:.0f}",
                ha="center", va="bottom", fontsize=7, color=TEXT)

    fig.tight_layout()
    _save(fig, out / "latency_summary.png")

# ── Figure 2: Per-benchmark latency histograms ────────────────────────────────

PRIMARY_BENCHES = [
    "BM_AddLimitOrder_NoMatch",
    "BM_AddLimitOrder_FullMatch",
    "BM_Cancel",
    "BM_HighContentionMatch",
]

def plot_histograms(data: dict[str, list[float]], out: Path) -> None:
    benches = [b for b in PRIMARY_BENCHES if b in data]
    mixed_key = choose_mixed_workload_key(data)
    if mixed_key:
        benches.append(mixed_key)
    if not benches:
        return

    n = len(benches)
    cols = min(n, 3)
    rows = (n + cols - 1) // cols

    fig, axes = plt.subplots(rows, cols, figsize=(cols * 4.5, rows * 3.5))
    axes = np.array(axes).flatten() if n > 1 else [axes]

    for i, (bm, ax) in enumerate(zip(benches, axes)):
        samples = np.array(data[bm])
        color   = SIDE_COLORS[i % len(SIDE_COLORS)]

        # Trim extreme outliers for readability (keep p0.5–p99.5)
        lo, hi = np.percentile(samples, [0.5, 99.5])
        trimmed = samples[(samples >= lo) & (samples <= hi)]

        ax.hist(trimmed, bins=40, color=color, alpha=0.8, edgecolor=BG, linewidth=0.4)

        st = stats(data[bm])
        for pct_val, pct_lbl, pct_col in [
            (st["median"], "p50", GREEN),
            (st["p95"],    "p95", AMBER),
            (st["p99"],    "p99", RED),
        ]:
            ax.axvline(pct_val, color=pct_col, linewidth=1.2, linestyle="--")
            ax.text(pct_val, ax.get_ylim()[1] * 0.02, f" {pct_lbl}",
                    color=pct_col, fontsize=7, va="bottom")

        ax.set_title(DISPLAY.get(bm, bm), fontsize=10)
        ax.set_xlabel("Latency (ns)", fontsize=8)
        ax.set_ylabel("Count", fontsize=8)
        ax.yaxis.grid(True, alpha=0.3)
        ax.set_axisbelow(True)

    # Hide unused subplots
    for ax in axes[n:]:
        ax.set_visible(False)

    fig.suptitle("Latency Distributions (p0.5–p99.5 trimmed)", y=1.01, fontsize=12)
    fig.tight_layout()
    _save(fig, out / "latency_histograms.png")

# ── Figure 3: Market order sweep scaling ─────────────────────────────────────

def plot_sweep_scaling(data: dict[str, list[float]], out: Path) -> None:
    sweep_keys = [
        ("BM_AddMarketOrder_SweepN/1",  1),
        ("BM_AddMarketOrder_SweepN/4",  4),
        ("BM_AddMarketOrder_SweepN/8",  8),
        ("BM_AddMarketOrder_SweepN/16", 16),
    ]
    available = [(k, n) for k, n in sweep_keys if k in data]
    if not available:
        return

    levels  = [n for _, n in available]
    medians = [stats(data[k])["median"] for k, _ in available]
    p99s    = [stats(data[k])["p99"]    for k, _ in available]

    fig, ax = plt.subplots(figsize=(7, 5))
    ax.plot(levels, medians, "o-", color=GREEN,  label="Median", linewidth=2)
    ax.plot(levels, p99s,    "s--", color=AMBER, label="p99",    linewidth=2)
    ax.fill_between(levels, medians, p99s, alpha=0.12, color=BLUE)

    ax.set_xlabel("Price levels swept")
    ax.set_ylabel("Latency (ns)")
    ax.set_title("Market Order Latency vs Levels Swept")
    ax.set_xticks(levels)
    ax.legend()
    ax.yaxis.grid(True, alpha=0.4)
    ax.set_axisbelow(True)

    # Annotate each point with ns value
    for x, y in zip(levels, medians):
        ax.annotate(f"{y:.0f} ns", (x, y), textcoords="offset points",
                    xytext=(4, 6), fontsize=8, color=GREEN)

    fig.tight_layout()
    _save(fig, out / "sweep_scaling.png")

# ── Figure 4: Throughput bar chart ────────────────────────────────────────────

def plot_throughput(data: dict[str, list[float]], meta: dict[str, float], out: Path) -> None:
    names = [n for n in DISPLAY if n in data]
    mixed_key = choose_mixed_workload_key(data)
    if mixed_key and mixed_key not in names:
        names.append(mixed_key)
    if not names:
        return

    tputs = [meta[n] / 1e6 if n in meta else 0.0 for n in names]
    labels = [DISPLAY[n].replace("\n", " ") if n in DISPLAY else n for n in names]

    # Sort descending
    pairs = sorted(zip(tputs, labels, names), reverse=True)
    tputs, labels, names = zip(*pairs)

    colors = [GREEN if t > 5 else AMBER if t > 1 else RED for t in tputs]

    fig, ax = plt.subplots(figsize=(10, 6))
    bars = ax.barh(labels, tputs, color=colors, alpha=0.88, edgecolor=BG, linewidth=0.4)

    for bar, val in zip(bars, tputs):
        ax.text(val + max(tputs) * 0.005, bar.get_y() + bar.get_height() / 2,
                f"{val:.2f}M", va="center", fontsize=8, color=TEXT)

    ax.set_xlabel("Throughput (million operations / second)")
    ax.set_title("Throughput by Operation Type")
    ax.xaxis.grid(True, alpha=0.4)
    ax.set_axisbelow(True)
    ax.invert_yaxis()

    fig.tight_layout()
    _save(fig, out / "throughput.png")

# ── Figure 5: Mixed workload CDF ──────────────────────────────────────────────

def plot_mixed_cdf(data: dict[str, list[float]], out: Path) -> None:
    key = choose_mixed_workload_key(data)
    if key is None:
        return

    samples = np.sort(data[key])
    cdf     = np.arange(1, len(samples) + 1) / len(samples)

    fig, ax = plt.subplots(figsize=FIGSIZE_SQUARE)
    ax.plot(samples, cdf * 100, color=BLUE, linewidth=2)
    ax.fill_between(samples, cdf * 100, alpha=0.15, color=BLUE)

    st = stats(data[key])
    for pct_ns, pct_lbl, col in [
        (st["median"], "p50",  GREEN),
        (st["p95"],    "p95",  AMBER),
        (st["p99"],    "p99",  RED),
        (st["max"],    "max",  "#ff6b6b"),
    ]:
        ax.axvline(pct_ns, color=col, linewidth=1, linestyle="--", alpha=0.8)
        ax.text(pct_ns, 2, f" {pct_lbl}\n {pct_ns:.0f}ns",
                color=col, fontsize=7, va="bottom")

    ax.set_xlabel("Latency (ns)")
    ax.set_ylabel("Percentile")
    ax.set_title("Mixed Workload — Latency CDF")
    ax.yaxis.set_major_formatter(mticker.PercentFormatter())
    ax.set_xlim(left=0, right=np.percentile(samples, 99.9))
    ax.yaxis.grid(True, alpha=0.4)
    ax.set_axisbelow(True)

    fig.tight_layout()
    _save(fig, out / "mixed_cdf.png")

# ── Figure 6: Mixed workload scaling — throughput and latency ───────────────

def plot_mixed_throughput(data: dict[str, list[float]], meta: dict[str, float], out: Path) -> None:
    entries = [
        (size, meta[name] / 1e6)
        for name in data
        if (size := extract_mixed_workload_size(name)) is not None and name in meta
    ]
    if len(entries) < 2:
        return

    entries.sort(key=lambda item: item[0])
    sizes = [size for size, _ in entries]
    throughputs = [value for _, value in entries]

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(sizes, throughputs, "o-", color=BLUE, linewidth=2)
    ax.set_xscale("log", base=10)
    ax.set_xticks(sizes)
    ax.set_xticklabels([f"{s//1000}k" if s < 1_000_000 else "1M" for s in sizes])
    ax.set_xlabel("Resting book size")
    ax.set_ylabel("Throughput (M messages / sec)")
    ax.set_title("Mixed Workload — Throughput vs Book Size")
    ax.yaxis.grid(True, alpha=0.4)
    ax.set_axisbelow(True)

    for x, y in zip(sizes, throughputs):
        ax.annotate(f"{y:.2f}M", (x, y), textcoords="offset points",
                    xytext=(0, 8), fontsize=8, color=BLUE, ha="center")

    fig.tight_layout()
    _save(fig, out / "mixed_throughput.png")


def plot_mixed_latency(data: dict[str, list[float]], out: Path) -> None:
    entries = [
        (size, stats(samples))
        for name, samples in data.items()
        if (size := extract_mixed_workload_size(name)) is not None
    ]
    if len(entries) < 2:
        return

    entries.sort(key=lambda item: item[0])
    sizes = [size for size, _ in entries]
    medians = [stats_["median"] / 1_000.0 for _, stats_ in entries]
    p95s = [stats_["p95"] / 1_000.0 for _, stats_ in entries]
    p99s = [stats_["p99"] / 1_000.0 for _, stats_ in entries]

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(sizes, medians, "o-", color=GREEN, label="p50")
    ax.plot(sizes, p95s,    "s--", color=AMBER, label="p95")
    ax.plot(sizes, p99s,    "d:", color=RED,   label="p99")

    ax.set_xscale("log", base=10)
    ax.set_xticks(sizes)
    ax.set_xticklabels([f"{s//1000}k" if s < 1_000_000 else "1M" for s in sizes])
    ax.set_xlabel("Resting book size")
    ax.set_ylabel("Latency (µs)")
    ax.set_title("Mixed Workload — Latency vs Book Size")
    ax.yaxis.grid(True, alpha=0.4)
    ax.set_axisbelow(True)
    ax.legend(loc="upper left")

    for x, y in zip(sizes, medians):
        ax.annotate(f"{y:.1f}µs", (x, y), textcoords="offset points",
                    xytext=(0, 6), fontsize=8, color=GREEN, ha="center")

    fig.tight_layout()
    _save(fig, out / "mixed_latency.png")

# ── Figure 7: FOK vs IOC comparison ──────────────────────────────────────────

def plot_fok_ioc(data: dict[str, list[float]], out: Path) -> None:
    cases = {
        "IOC":           "BM_AddIOC",
        "FOK (fillable)":"BM_AddFOK_Fillable",
        "FOK (cancelled)":"BM_AddFOK_Cancelled",
    }
    available = {label: key for label, key in cases.items() if key in data}
    if not available:
        return

    fig, axes = plt.subplots(1, 3, figsize=(12, 4.5), sharey=False)
    colors = [TEAL, GREEN, AMBER]

    for ax, (label, key), color in zip(axes, available.items(), colors):
        samples = np.array(data[key])
        lo, hi  = np.percentile(samples, [0.5, 99.5])
        trimmed = samples[(samples >= lo) & (samples <= hi)]
        ax.hist(trimmed, bins=35, color=color, alpha=0.85, edgecolor=BG, linewidth=0.4)

        st = stats(data[key])
        ax.axvline(st["median"], color=TEXT, linewidth=1.2, linestyle="--")
        ax.set_title(label, fontsize=10)
        ax.set_xlabel("Latency (ns)", fontsize=8)
        ax.set_ylabel("Count", fontsize=8)

        # Stat box
        box_text = (
            f"p50  {st['median']:.0f}ns\n"
            f"p95  {st['p95']:.0f}ns\n"
            f"p99  {st['p99']:.0f}ns\n"
            f"max  {st['max']:.0f}ns"
        )
        ax.text(0.97, 0.97, box_text, transform=ax.transAxes,
                fontsize=7, va="top", ha="right", color=TEXT,
                bbox=dict(facecolor=BG, edgecolor=BORDER, boxstyle="round,pad=0.4"))
        ax.yaxis.grid(True, alpha=0.3)
        ax.set_axisbelow(True)

    fig.suptitle("IOC vs FOK Latency Comparison", fontsize=12)
    fig.tight_layout()
    _save(fig, out / "fok_ioc_comparison.png")

# ── Figure 7: Snapshot depth scaling ─────────────────────────────────────────

def plot_snapshot_scaling(data: dict[str, list[float]], out: Path) -> None:
    snap_keys = [
        ("BM_Snapshot/5",  5),
        ("BM_Snapshot/10", 10),
        ("BM_Snapshot/20", 20),
    ]
    available = [(k, d) for k, d in snap_keys if k in data]
    if len(available) < 2:
        return

    depths  = [d for _, d in available]
    medians = [stats(data[k])["median"] for k, _ in available]
    p99s    = [stats(data[k])["p99"]    for k, _ in available]

    fig, ax = plt.subplots(figsize=(6, 4.5))
    ax.plot(depths, medians, "o-", color=PURPLE, label="Median", linewidth=2)
    ax.plot(depths, p99s,    "s--", color=RED,   label="p99",    linewidth=2)

    ax.set_xlabel("Snapshot depth (levels per side)")
    ax.set_ylabel("Latency (ns)")
    ax.set_title("Snapshot Latency vs Book Depth")
    ax.set_xticks(depths)
    ax.legend()
    ax.yaxis.grid(True, alpha=0.4)
    ax.set_axisbelow(True)

    for x, y in zip(depths, medians):
        ax.annotate(f"{y:.0f} ns", (x, y), textcoords="offset points",
                    xytext=(4, 5), fontsize=8, color=PURPLE)

    fig.tight_layout()
    _save(fig, out / "snapshot_scaling.png")

# ── Figure 8: Memory per order (static analysis) ─────────────────────────────

def plot_memory_estimate(out: Path) -> None:
    """
    Plots a breakdown of per-order memory cost derived from sizeof() knowledge.
    Static analysis — no runtime data needed.
    These are conservative estimates for a 64-bit system.
    """
    components = {
        "Order struct\n(id, symbol, price, qty…)": 88,    # sizeof(Order) ≈ 88B
        "std::list node\noverhead (prev/next)":     16,    # 2× pointer
        "order_map_ entry\n(id → OrderLocation)":  48,    # pair + hash bucket
        "PriceLevel total_qty_\naccounting":         8,    # uint64_t
        "std::map node\n(new price level amort.)":  40,    # amortised, 40B/order at 5 per level
    }
    labels = list(components.keys())
    sizes  = list(components.values())
    total  = sum(sizes)

    colors_pie = [GREEN, BLUE, AMBER, PURPLE, TEAL]

    fig, (ax_pie, ax_bar) = plt.subplots(1, 2, figsize=(12, 5))

    # Pie
    wedges, texts, autotexts = ax_pie.pie(
        sizes, labels=labels, autopct="%1.0f%%",
        colors=colors_pie, startangle=90,
        textprops={"color": TEXT, "fontsize": 7.5},
        wedgeprops={"edgecolor": BG, "linewidth": 0.8},
    )
    for at in autotexts:
        at.set_color(BG)
        at.set_fontsize(8)
    ax_pie.set_title(f"Per-Order Memory Breakdown\n(total ≈ {total}B / order)", pad=8)

    # Bar
    bars = ax_bar.barh(labels, sizes, color=colors_pie, alpha=0.88,
                       edgecolor=BG, linewidth=0.4)
    for bar, val in zip(bars, sizes):
        ax_bar.text(val + 1, bar.get_y() + bar.get_height() / 2,
                    f"{val}B", va="center", fontsize=8, color=TEXT)
    ax_bar.set_xlabel("Bytes")
    ax_bar.set_title("Component Breakdown")
    ax_bar.xaxis.grid(True, alpha=0.4)
    ax_bar.set_axisbelow(True)
    ax_bar.invert_yaxis()
    ax_bar.tick_params(axis="y", labelsize=7.5)

    fig.suptitle("TreeOrderBook Memory Cost Analysis", fontsize=12, y=1.01)
    fig.tight_layout()
    _save(fig, out / "memory_estimate.png")

# ── Markdown stats table ───────────────────────────────────────────────────────

def write_stats_table(data: dict[str, list[float]], meta: dict[str, float], out: Path) -> None:
    lines = [
        "## Benchmark Results\n",
        "| Operation | n | Median (ns) | p95 (ns) | p99 (ns) | Max (ns) | Throughput |",
        "|-----------|---|-------------|----------|----------|----------|------------|",
    ]
    for name in DISPLAY:
        if name not in data:
            continue
        s = stats(data[name])
        tput_val = meta.get(name, 0.0)
        tput_str = format_tput(tput_val)
        lines.append(
            f"| {DISPLAY[name].replace(chr(10), ' ')} "
            f"| {s['n']} "
            f"| {s['median']:.1f} "
            f"| {s['p95']:.1f} "
            f"| {s['p99']:.1f} "
            f"| {s['max']:.1f} "
            f"| {tput_str} |"
        )

    mixed_keys = sorted(
        (name for name in data if name.startswith("BM_MixedWorkload/")),
        key=lambda key: extract_mixed_workload_size(key) or 0,
    )
    for name in mixed_keys:
        s = stats(data[name])
        size = extract_mixed_workload_size(name)
        label = f"Mixed workload ({size//1000}k resting orders)" if size else "Mixed workload"
        tput_val = meta.get(name, 0.0)
        tput_str = format_tput(tput_val)
        lines.append(
            f"| {label} "
            f"| {s['n']} "
            f"| {s['median']:.1f} "
            f"| {s['p95']:.1f} "
            f"| {s['p99']:.1f} "
            f"| {s['max']:.1f} "
            f"| {tput_str} |"
        )

    table_path = out / "stats_table.md"
    table_path.write_text("\n".join(lines) + "\n")
    print(f"  wrote {table_path}")

# ── Entry point ────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate benchmark plots from Google Benchmark JSON output."
    )
    parser.add_argument(
        "json_path",
        nargs="?",
        default="docs/bench_raw.json",
        help="Path to bench_raw.json (default: docs/bench_raw.json)",
    )
    parser.add_argument(
        "--out",
        default="docs/bench_figures",
        help="Output directory for figures (default: docs/bench_figures)",
    )
    args = parser.parse_args()

    json_path = Path(args.json_path)
    out       = Path(args.out)

    if not json_path.exists():
        print(f"ERROR: {json_path} not found.")
        print()
        print("Run the benchmark first:")
        print("  ./build/benchmarks/bench_order_book \\")
        print("      --benchmark_repetitions=20 \\")
        print("      --benchmark_report_aggregates_only=false \\")
        print("      --benchmark_format=json \\")
        print(f"      --benchmark_out={json_path}")
        sys.exit(1)

    out.mkdir(parents=True, exist_ok=True)
    _style()

    print(f"Loading {json_path}…")
    data, meta = load(json_path)
    print(f"  {len(data)} benchmark cases found: {', '.join(sorted(data))}")
    print(f"Writing figures to {out}/")

    plot_latency_summary(data, out)
    plot_histograms(data, out)
    plot_sweep_scaling(data, out)
    plot_throughput(data, meta, out)
    plot_mixed_cdf(data, out)
    plot_mixed_throughput(data, meta, out)
    plot_mixed_latency(data, out)
    plot_fok_ioc(data, out)
    plot_snapshot_scaling(data, out)
    plot_memory_estimate(out)
    write_stats_table(data, meta, out)

    print()
    print("Done. Embed in README.md with:")
    print("  ![Latency Summary](docs/bench_figures/latency_summary.png)")
    print("  ![Throughput](docs/bench_figures/throughput.png)")
    print("  ![Mixed Workload CDF](docs/bench_figures/mixed_cdf.png)")
    print("  ![Mixed Workload Throughput](docs/bench_figures/mixed_throughput.png)")
    print("  ![Mixed Workload Latency](docs/bench_figures/mixed_latency.png)")

if __name__ == "__main__":
    main()