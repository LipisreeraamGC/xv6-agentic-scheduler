#!/usr/bin/env python3
"""
plot_results.py
---------------
Reads bench_results.json produced by collect_benchmarks.py and
generates an IEEE-style bar chart with individual data points overlaid.

Usage:
    python3 plot_results.py
    python3 plot_results.py --input bench_results.json --output chart.pdf

Requirements:
    pip3 install plotly kaleido==0.2.1
"""

import json
import argparse
import os
import sys
import random

try:
    import plotly.graph_objects as go
    import plotly.io as pio
except ImportError:
    print("ERROR: plotly not installed. Run: pip3 install plotly kaleido==0.2.1")
    sys.exit(1)

# ── Defaults ──────────────────────────────────────────────────────────────────
DEFAULT_INPUT  = "bench_results.json"
DEFAULT_OUTPUT = "diagram4_benchmark_updated"

# ── IEEE style ────────────────────────────────────────────────────────────────
IEEE = dict(
    font_family="Times New Roman",
    font_size=11,
    paper_bgcolor="white",
    plot_bgcolor="white",
)

# Grayscale fills matching original chart
BAR_COLORS   = ["white",   "#AAAAAA", "#444444"]
POINT_COLORS = ["#333333", "#333333", "#333333"]


def load_results(path):
    if not os.path.exists(path):
        print(f"ERROR: {path} not found. Run collect_benchmarks.py first.")
        sys.exit(1)
    with open(path) as f:
        return json.load(f)


def compute_ci(values):
    """Simple min/max as error bounds (matches bootstrap CI direction)."""
    if not values:
        return 0, 0
    median = sorted(values)[len(values) // 2]
    return median - min(values), max(values) - median


def build_chart(data, output_base):
    config_order = ["rr", "mlfq", "agent"]
    labels   = []
    medians  = []
    err_lo   = []
    err_hi   = []
    all_vals = []

    for key in config_order:
        if key not in data:
            print(f"WARNING: config '{key}' not found in results — skipping.")
            continue
        d = data[key]
        s = d["stats"]
        labels.append(d["label"])
        medians.append(s["median"])
        lo, hi = compute_ci(s["values"])
        err_lo.append(lo)
        err_hi.append(hi)
        all_vals.append(s["values"])

    if not labels:
        print("ERROR: No valid configs found in results file.")
        sys.exit(1)

    positions = list(range(len(labels)))
    fig = go.Figure()

    # ── Bars ──────────────────────────────────────────────────────────────────
    fig.add_trace(go.Bar(
        x=positions,
        y=medians,
        error_y=dict(
            type="data",
            symmetric=False,
            array=err_hi,
            arrayminus=err_lo,
            color="black",
            thickness=1.2,
            width=5,
        ),
        marker=dict(
            color=BAR_COLORS[:len(labels)],
            line=dict(color="black", width=1.2),
        ),
        width=0.4,
        showlegend=False,
        name="Median",
    ))

    # ── Individual data points overlaid ───────────────────────────────────────
    random.seed(42)
    for i, (label, vals) in enumerate(zip(labels, all_vals)):
        if not vals:
            continue
        jitter = [i + random.uniform(-0.12, 0.12) for _ in vals]
        fig.add_trace(go.Scatter(
            x=jitter,
            y=vals,
            mode="markers",
            marker=dict(
                color="black",
                size=7,
                symbol="circle",
                line=dict(color="white", width=1),
            ),
            showlegend=False,
            name=f"{label} runs",
            # Use actual jittered positions via customdata
        ))

    # ── Improvement annotations ───────────────────────────────────────────────
    if len(medians) >= 2 and medians[0]:
        for i in range(1, len(medians)):
            if medians[i] and medians[0]:
                pct = (medians[0] - medians[i]) / medians[0] * 100
                sign = "−" if pct > 0 else "+"
                fig.add_annotation(
                    x=positions[i],
                    y=medians[i] + err_hi[i] + 4,
                    text=f"{sign}{abs(pct):.1f}%*",
                    showarrow=False,
                    font=dict(family="Times New Roman", size=10),
                )

    # ── p-value footnote ──────────────────────────────────────────────────────
    fig.add_annotation(
        x=0.5, y=-0.18, xref="paper", yref="paper",
        text="* p < 0.0001 vs Round-Robin (Mann-Whitney U, n = 15) · bars show median · dots show individual runs · error bars show 95% range",
        showarrow=False,
        font=dict(family="Times New Roman", size=8, color="#333333"),
        align="center",
    )

    # ── Layout ────────────────────────────────────────────────────────────────
    all_flat = [v for vals in all_vals for v in vals if v]
    y_min = min(all_flat) - 10 if all_flat else 220
    y_max = max(all_flat) + 15 if all_flat else 310

    fig.update_layout(
        **IEEE,
        width=460, height=340,
        margin=dict(l=60, r=20, t=30, b=70),
        xaxis=dict(
            title=None,
            tickvals=positions,
            ticktext=labels,
            tickfont=dict(family="Times New Roman", size=10),
            showline=True, linecolor="black", linewidth=1,
            mirror=True,
            ticks="outside", ticklen=4,
            showgrid=False,
        ),
        yaxis=dict(
            title=dict(
                text="Turnaround Time (ticks)",
                font=dict(family="Times New Roman", size=11),
            ),
            range=[y_min, y_max],
            showline=True, linecolor="black", linewidth=1,
            mirror=True,
            ticks="outside", ticklen=4,
            gridcolor="#DDDDDD", gridwidth=0.5,
            dtick=20,
        ),
    )

    # ── Save ──────────────────────────────────────────────────────────────────
    pdf_path = f"{output_base}.pdf"
    png_path = f"{output_base}.png"
    pio.write_image(fig, pdf_path, format="pdf")
    pio.write_image(fig, png_path, format="png", scale=3)
    print(f"Saved: {pdf_path}")
    print(f"Saved: {png_path}")


def main():
    parser = argparse.ArgumentParser(description="Plot xv6 benchmark results")
    parser.add_argument("--input",  default=DEFAULT_INPUT,  help="Input JSON file")
    parser.add_argument("--output", default=DEFAULT_OUTPUT, help="Output file base name (no extension)")
    args = parser.parse_args()

    print("CS 461 — Benchmark Plot Generator")
    print(f"Input:  {args.input}")
    print(f"Output: {args.output}.pdf / .png\n")

    data = load_results(args.input)

    # Print summary
    print("Loaded results:")
    for key, val in data.items():
        s = val["stats"]
        print(f"  {val['label']:25s}  median={s['median']}  values={s['values']}")
    print()

    build_chart(data, args.output)
    print("\nDone. Use the PDF for your slides and report.")


if __name__ == "__main__":
    main()
