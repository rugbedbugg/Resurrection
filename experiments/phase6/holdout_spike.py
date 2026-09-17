#!/usr/bin/env python3
"""Phase 6A spike: how well does a held-out sample estimate prefetch precision?

A prefetched page is resident and reveals nothing about whether it was read.
Withhold k pages of the prediction instead: they stay absent, so they fault iff
the workload still wants them, and the fault rate over the sample estimates

    precision = |predicted and actually touched| / |predicted|

which is otherwise unobservable. This measures the estimator's error, its
spread across seeds, and what the extra faults cost -- nothing here is policy.

Not a benchmark: single runs, no interleaving. Latency figures are indicative.
"""
import argparse, os, pathlib, random, shutil, statistics, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
ARCTL = ROOT / "build" / "arctl"
WL = ROOT / "build" / "workloads" / "workload_sudden_phase_change"


def run(cmd, env=None, cwd=None):
    e = dict(os.environ)
    if env:
        e.update({k: str(v) for k, v in env.items()})
    return subprocess.run(cmd, capture_output=True, text=True, env=e, cwd=cwd)


def field(text, key):
    for line in text.splitlines():
        s = line.strip()
        if s.startswith(key):
            return s[len(key):].strip()
    return ""


def read_report(path):
    """-> {region: {page: (held_out, faulted)}}"""
    out = {}
    with open(path) as f:
        next(f)
        for line in f:
            r, p, _va, _pred, held, fault = line.split("\t")
            out.setdefault(int(r), {})[int(p)] = (int(held), int(fault))
    return out


def sampled_per_region(rep):
    """p_hat per region, from held-out pages only.

    Only a held-out page carries evidence: a written page is resident and a
    read of it is invisible. Aggregating hits/total across regions would
    estimate the UNWEIGHTED mean of per-region precisions, which is a different
    quantity from the page-weighted precision -- so keep it per region, which
    is also the granularity a planner decides at."""
    out = {}
    for r, pages in rep.items():
        hit = tot = 0
        for _p, (held, fault) in pages.items():
            if held:
                tot += 1
                hit += fault
        if tot:
            out[r] = hit / tot
    return out


def truth_per_region(rep, touched, vaddr_of):
    """Exact per-region precision over ALL predicted pages."""
    out = {}
    for r, pages in rep.items():
        hit = tot = 0
        for p in pages:
            tot += 1
            hit += vaddr_of[(r, p)] in touched
        if tot:
            out[r] = hit / tot
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase", default="B", choices=["A", "B"],
                    help="phase to restore into; profile is always taught on A")
    ap.add_argument("--ks", default="1,2,4,8,16")
    ap.add_argument("--seeds", type=int, default=12)
    ap.add_argument("--workdir", default=None)
    a = ap.parse_args()

    tmp = a.workdir or tempfile.mkdtemp(prefix="ar-phase6-")
    d = pathlib.Path(tmp); d.mkdir(parents=True, exist_ok=True)
    ctl, img, prof = d / "phase.ctl", d / "spike.arimg", d / "spike.db"

    ctl.write_text("A")
    r = run([str(ARCTL), "checkpoint", "--output", str(img), "--", str(WL), str(ctl)])
    if r.returncode:
        sys.exit("checkpoint failed:\n" + r.stdout + r.stderr)

    # Teach the prediction under phase A.
    run([str(ARCTL), "restore", "--image", str(img), "--policy", "lazy",
         "--profile", str(prof)])

    # Ground truth for the phase we will measure, from a pure-lazy run.
    ctl.write_text(a.phase)
    tr = d / "truth.tsv"
    run([str(ARCTL), "restore", "--image", str(img), "--policy", "lazy",
         "--record-trace", str(tr)])
    touched = set()
    with open(tr) as f:
        next(f)
        for line in f:
            touched.add(int(line.split("\t")[0], 16))

    # Baseline: prefetch with no holdout, for the latency reference.
    rep0 = d / "rep0.tsv"
    base = run([str(ARCTL), "restore", "--image", str(img), "--policy", "prefetch",
                "--profile", str(prof), "--no-learn"],
               env={"AR_EXPERIMENTAL_HOLDOUT_REPORT": str(rep0)})
    base_resume = float(field(base.stdout, "resume").split()[0])
    base_faults = int(field(base.stdout, "faults").split()[0])

    def service_ns(text):
        """Total time inside the fault handler. Far less noisy than resume
        latency, and it is the quantity a planner would actually have to pay."""
        f = field(text, "latency")
        if not f:
            return 0.0
        # "<mean> ns mean per fault, <total> ns total"
        return float(f.split(",")[1].strip().split()[0])

    def total_ms(text):
        """What the benchmark harness calls restore cost: time to first
        instruction PLUS the fault service paid afterwards. Withholding pages
        moves work from the first term into the second, so only the sum is a
        fair comparison."""
        r = field(text, "resume")
        return (float(r.split()[0]) if r else 0.0) + service_ns(text) / 1e6

    base_service = service_ns(base.stdout)
    base_total = total_ms(base.stdout)
    r0 = read_report(rep0)
    vaddr_of = {}
    with open(rep0) as f:
        next(f)
        for line in f:
            rr, pp, va, *_ = line.split("\t")
            vaddr_of[(int(rr), int(pp))] = int(va)
    predicted_total = sum(len(v) for v in r0.values())
    truth = truth_per_region(r0, touched, vaddr_of)
    true_p = sum(len(v) for v in r0.values() and [1]) if False else (
        sum(truth[r] * len(r0[r]) for r in truth) / predicted_total)
    true_unw = statistics.mean(truth.values())

    print(f"# workdir {d}")
    print(f"# phase measured        {a.phase}   (profile taught on A)")
    print(f"# predicted pages       {predicted_total} across {len(r0)} regions")
    print(f"# TRUE precision        {true_p:.4f} page-weighted, "
          f"{true_unw:.4f} unweighted region mean")
    sizes = sorted(len(v) for v in r0.values())
    print(f"# predicted per region  min {sizes[0]}, median {sizes[len(sizes)//2]}, max {sizes[-1]}")
    print(f"# baseline k=0          resume {base_resume:.2f} ms, {base_faults} faults\n")

    print(f"{'k':>3} {'seeds':>5} {'mean_est':>9} {'bias':>8} {'MAE/reg':>8} "
          f"{'sd_seed':>7} {'extra_flt':>9} {'pred_ms':>8} {'total_d':>9} {'resume_d':>9}")
    MEAN_FAULT_US = 26.2   # calibration.cpp defaults
    for k in [int(x) for x in a.ks.split(",")]:
        ests, extra_f, extra_ms, extra_svc, per_region_err = [], [], [], [], []
        for s in range(a.seeds):
            rep = d / f"rep_k{k}_s{s}.tsv"
            rr = run([str(ARCTL), "restore", "--image", str(img), "--policy", "prefetch",
                      "--profile", str(prof), "--no-learn"],
                     env={"AR_EXPERIMENTAL_HOLDOUT_K": k,
                          "AR_EXPERIMENTAL_HOLDOUT_SEED": 0x5EED0000 + s,
                          "AR_EXPERIMENTAL_HOLDOUT_REPORT": str(rep)})
            if rr.returncode:
                sys.exit(f"restore k={k} s={s} failed:\n{rr.stdout}{rr.stderr}")
            est = sampled_per_region(read_report(rep))
            # Per-region absolute error against exact per-region truth.
            for rr_id, ph in est.items():
                if rr_id in truth:
                    per_region_err.append(abs(ph - truth[rr_id]))
            if est:
                ests.append(statistics.mean(est.values()))
            extra_f.append(int(field(rr.stdout, "faults").split()[0]) - base_faults)
            extra_ms.append(float(field(rr.stdout, "resume").split()[0]) - base_resume)
            extra_svc.append(total_ms(rr.stdout) - base_total)
            rep.unlink(missing_ok=True)
        if not ests:
            continue
        sd = statistics.pstdev(ests) if len(ests) > 1 else 0.0
        lo, hi = min(ests), max(ests)
        mf = statistics.mean(extra_f)
        mae = statistics.mean(per_region_err) if per_region_err else float("nan")
        print(f"{k:>3} {len(ests):>5} {statistics.mean(ests):>9.4f} "
              f"{statistics.mean(ests)-true_unw:>+8.4f} {mae:>8.4f} {sd:>7.4f} "
              f"{mf:>9.0f} {mf*MEAN_FAULT_US/1000:>8.2f} "
              f"{statistics.mean(extra_svc):>9.2f} {statistics.mean(extra_ms):>9.2f}")


if __name__ == "__main__":
    main()
