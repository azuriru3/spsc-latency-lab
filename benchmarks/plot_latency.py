"""Renders the two CSVs bench_latency writes into a tail-latency comparison
chart. Run bench_latency first (from the repo root, so the relative
benchmarks/results/ paths resolve), then:

    python benchmarks/plot_latency.py
"""

import csv
from pathlib import Path

import matplotlib.pyplot as plt

RESULTS_DIR = Path(__file__).parent / "results"


def load_latencies_us(name: str) -> list[float]:
    path = RESULTS_DIR / f"{name}.csv"
    with path.open() as f:
        reader = csv.DictReader(f)
        return [int(row["latency_ns"]) / 1000 for row in reader]


def percentile(sorted_values: list[float], p: float) -> float:
    idx = int(p * (len(sorted_values) - 1))
    return sorted_values[idx]


def main() -> None:
    lockfree = sorted(load_latencies_us("lockfree"))
    mutex = sorted(load_latencies_us("mutex"))

    fig, (ax_hist, ax_tail) = plt.subplots(1, 2, figsize=(12, 5))

    ax_hist.hist(lockfree, bins=100, alpha=0.6, label="SpscRing (lock-free)", log=True)
    ax_hist.hist(mutex, bins=100, alpha=0.6, label="MutexRing (mutex + condvar)", log=True)
    ax_hist.set_xlabel("latency (us)")
    ax_hist.set_ylabel("count (log scale)")
    ax_hist.set_title("Producer -> consumer latency distribution")
    ax_hist.legend()

    percentiles = [0.50, 0.90, 0.99, 0.999, 0.9999]
    labels = ["p50", "p90", "p99", "p99.9", "p99.99"]
    lockfree_p = [percentile(lockfree, p) for p in percentiles]
    mutex_p = [percentile(mutex, p) for p in percentiles]

    ax_tail.plot(labels, lockfree_p, marker="o", label="SpscRing (lock-free)")
    ax_tail.plot(labels, mutex_p, marker="o", label="MutexRing (mutex + condvar)")
    ax_tail.set_yscale("log")
    ax_tail.set_ylabel("latency (us, log scale)")
    ax_tail.set_title("Tail latency: this is the whole argument")
    ax_tail.legend()

    fig.tight_layout()
    out_path = RESULTS_DIR / "latency_comparison.png"
    fig.savefig(out_path, dpi=150)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
