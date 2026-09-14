#!/usr/bin/env python3
"""Drive the full all-reduce benchmark matrix from a single terminal.

Run this ON the first host of the list (default mlx-stud-01). Rank 1 runs
locally; every other rank is launched over ssh, relying on the shared NFS
home so the same repo path and ./test binary exist on every node. Rank 1's
stdout (the TSV) lands in results-<nodes>-<label>.tsv in the repo root.

One-time setup for passwordless ssh between the lab machines (shared home,
so authorizing your own key once covers all of them):

    ssh-keygen -t ed25519 -N ''
    cat ~/.ssh/id_ed25519.pub >> ~/.ssh/authorized_keys

Examples:
    python3 run_benchmarks.py                          # 2- and 4-node, all modes
    python3 run_benchmarks.py --nodes 2                # 2-node half only
    python3 run_benchmarks.py --sanity                 # correctness suite first
    python3 run_benchmarks.py --sweep-thresholds 8192 16384 32768 65536
"""

import argparse
import shlex
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent
DEFAULT_HOSTS = ["mlx-stud-01", "mlx-stud-02", "mlx-stud-03", "mlx-stud-04"]
MODES = {
    "auto": ["-mode", "auto"],
    "eager": ["-mode", "eager"],
    "rdvz": ["-mode", "rdvz"],
    "rdvz-nopipe": ["-mode", "rdvz", "-nopipe"],
}
RUN_TIMEOUT = 1800
SSH_OPTS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]


def repo_on_remote():
    """Path of the repo as seen from the other nodes (shared $HOME)."""
    try:
        return "$HOME/" + shlex.quote(str(REPO.relative_to(Path.home())))
    except ValueError:
        return shlex.quote(str(REPO))


def start_rank(rank, hosts, action_args, stdout):
    """Start one rank (1-based). Rank 1 is local, others go through ssh."""
    cmd = ["./test", "-myindex", str(rank), "-list", *hosts, *action_args]
    if rank == 1:
        return subprocess.Popen(cmd, cwd=REPO, stdout=stdout,
                                stderr=subprocess.PIPE)
    remote = 'cd "%s" && exec %s' % (repo_on_remote(), shlex.join(cmd))
    return subprocess.Popen(["ssh", *SSH_OPTS, hosts[rank - 1], remote],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)


def kill_stragglers(hosts):
    """Best-effort cleanup so a wedged run does not poison the next one."""
    subprocess.run(["pkill", "-f", "./test -myindex"],
                   stderr=subprocess.DEVNULL, check=False)
    for host in hosts[1:]:
        subprocess.run(["ssh", *SSH_OPTS, host, "pkill -f './test -myindex'"],
                       stderr=subprocess.DEVNULL, check=False)


def run_group(hosts, action_args, outfile=None):
    """Run one collective job across len(hosts) ranks; True on success."""
    sink = open(outfile, "wb") if outfile else None
    procs = []
    try:
        # Remote ranks first: they sit in the bootstrap retry loop until
        # rank 1 joins, so start order never races the 60 s window.
        for rank in range(2, len(hosts) + 1):
            procs.append(start_rank(rank, hosts, action_args, None))
        procs.insert(0, start_rank(1, hosts, action_args,
                                   sink if sink else None))
        deadline = time.monotonic() + RUN_TIMEOUT
        failed = False
        for rank, proc in enumerate(procs, start=1):
            remaining = max(1.0, deadline - time.monotonic())
            try:
                _, err = proc.communicate(timeout=remaining)
            except subprocess.TimeoutExpired:
                proc.kill()
                _, err = proc.communicate()
                failed = True
            if proc.returncode != 0:
                failed = True
                tail = (err or b"").decode(errors="replace").strip()
                print("  rank %d failed (rc=%s)" % (rank, proc.returncode))
                if tail:
                    print("    " + "\n    ".join(tail.splitlines()[-8:]))
        if failed:
            kill_stragglers(hosts)
        return not failed
    finally:
        if sink:
            sink.close()


def check_load(hosts):
    print("== load check ==")
    for host in hosts:
        if host == hosts[0]:
            out = subprocess.run(["uptime"], capture_output=True, text=True)
        else:
            out = subprocess.run(["ssh", *SSH_OPTS, host, "uptime"],
                                 capture_output=True, text=True)
        line = (out.stdout or out.stderr).strip() or "unreachable"
        print("  %-12s %s" % (host, line))
    print("(1-min load well above the core count means someone is computing;"
          " prefer a quieter time)")


def summarize(outfile):
    lines = Path(outfile).read_text().strip().splitlines()
    rows = [l for l in lines if l and not l.startswith("#")]
    tag = "%d rows" % len(rows)
    if rows:
        tag += ", 4MB row: " + rows[-1].replace("\t", "  ")
    print("  -> %s (%s)" % (outfile, tag))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--hosts", nargs="+", default=DEFAULT_HOSTS,
                        help="ring hosts in order; run this script on the first")
    parser.add_argument("--nodes", nargs="+", type=int, default=[2, 4],
                        help="group sizes to benchmark (prefixes of --hosts)")
    parser.add_argument("--modes", nargs="+", default=list(MODES),
                        choices=list(MODES), help="transport configurations")
    parser.add_argument("--iters", type=int, default=0,
                        help="override iterations per size (0 = banded default)")
    parser.add_argument("--sanity", action="store_true",
                        help="run correctness suites for every selected group size")
    parser.add_argument("--sweep-thresholds", nargs="+", type=int, default=[],
                        metavar="BYTES",
                        help="extra 2-node auto runs with -threshold values")
    parser.add_argument("--skip-load-check", action="store_true")
    args = parser.parse_args()

    if max(args.nodes) > len(args.hosts):
        sys.exit("--nodes %s needs at least that many --hosts" % args.nodes)
    me = socket.gethostname().split(".")[0]
    if me != args.hosts[0]:
        print("warning: running on %s but rank 1 belongs to %s"
              % (me, args.hosts[0]))

    print("== build ==")
    if subprocess.run(["make"], cwd=REPO).returncode != 0:
        sys.exit("make failed")

    if not args.skip_load_check:
        check_load(args.hosts[:max(args.nodes)])

    iters = ["-iters", str(args.iters)] if args.iters > 0 else []

    if args.sanity:
        print("== sanity: -suite (auto, then forced eager/rdvz) ==")
        for n in sorted(args.nodes):
            for extra in ([], ["-mode", "eager"], ["-mode", "rdvz"]):
                if not run_group(args.hosts[:n], ["-suite", *extra]):
                    sys.exit("%d-node correctness suite failed; not benchmarking" % n)
            print("  %d-node suites passed" % n)

    jobs = []
    for n in sorted(args.nodes):
        for mode in args.modes:
            jobs.append((n, MODES[mode] + iters, "results-%d-%s.tsv" % (n, mode)))
    for threshold in args.sweep_thresholds:
        jobs.append((2, MODES["auto"] + ["-threshold", str(threshold)] + iters,
                     "results-2-thresh-%d.tsv" % threshold))

    print("== benchmarks: %d runs ==" % len(jobs))
    for n, extra, outfile in jobs:
        started = time.monotonic()
        print("[%d nodes] %s" % (n, " ".join(extra) or "auto"))
        if not run_group(args.hosts[:n], ["-bench", *extra], outfile):
            sys.exit("benchmark run failed: %s" % outfile)
        summarize(outfile)
        print("  took %.0f s" % (time.monotonic() - started))
        time.sleep(2)  # let bootstrap listeners fully close between runs

    print("all runs complete; commit the results-*.tsv files")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\ninterrupted; cleaning up remote ranks")
        kill_stragglers(DEFAULT_HOSTS)
        sys.exit(130)
