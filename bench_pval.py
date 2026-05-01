from scipy.stats import mannwhitneyu
import json

with open("bench_results.json") as f:
    d = json.load(f)

rr   = d["rr"]["stats"]["values"]
mlfq = d["mlfq"]["stats"]["values"]

stat, p = mannwhitneyu(rr, mlfq, alternative="greater")
print(f"RR values:   {rr}")
print(f"MLFQ values: {mlfq}")
print(f"p = {p:.4f}")
