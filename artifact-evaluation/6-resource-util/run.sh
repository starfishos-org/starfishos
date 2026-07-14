#!/usr/bin/env bash
#
# Artifact script for the paper "real" figure (real.eps): 12 applications, each
# measured under three conditions and normalized to its own single-run.
#
#   single  — the application alone on one machine (baseline)
#   stress  — co-located with a competing workload, traditional DRAM placement
#   p3os    — co-located under StarfishOS (the competitor is offloaded to a
#             second machine over CXL/DSM)
#
# The co-location pairs are the ones already encoded in
#   user/script/single_stress_type{1..6}.sh   (traditional, one machine)
#   user/script/cross_stress_type{1..4}_m{0,1}.sh (StarfishOS, two machines)
# and driven manually by dsm-scripts/tests/real/{dram,cxl}.sh.  This script
# wraps that flow in the AE harness, captures one <bench>_<cond>.log per
# (application, condition), then parses + plots.
#
# Usage (from repo root):
#   ./artifact-evaluation/prepare.sh          # once
#   ./artifact-evaluation/9-resource-util/run.sh
#
# Prerequisites (user/demos/config.cmake): the paper set needs LEVELDB, PHOENIX,
# DBX1000, GEIMINIGRAPH (currently ON) plus REDIS, MEMCACHED/MEMCACHETEST and
# TINYCNN (currently OFF — enable + rebuild before a full run).
#
# ###########################################################################
# ## CAVEAT — NOT YET VALIDATED AGAINST A LIVE RUN.
# ## The (application -> stress type) demux and the per-application completion
# ## markers below are derived from the stress scripts and config.exp, not from
# ## an executed co-location run.  Before trusting the numbers:
# ##   1. confirm which application's output lands in which log and how each
# ##      one is demuxed into a <bench>_<cond>.log (metrics differ per app);
# ##   2. confirm the completion markers (COND_MARKER);
# ##   3. cross-check plot.py's EXTRACTORS against real app output.
# ## The plotting path IS validated: it reproduces real.eps from the paper CSV.
# ###########################################################################
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/6-resource-util"
TS="${TS:-$(date +%Y%m%d_%H%M%S)}"
OUT_DIR="${OUT_DIR:-$AE_DIR/out/$TS}"
AE_LOG_DIR="$OUT_DIR/logs"
TIMEOUT="${TIMEOUT:-1800}"

# The 6 co-location groups (paper dram_groups order). Each pair shares one host
# under "stress"/traditional and is split across two hosts under "p3os".
STRESS_TYPES="${STRESS_TYPES:-1 2 3 4 5 6}"
# Conditions to run (single baselines are cheap and per-app; the co-location
# ones reuse the stress scripts).
CONDS="${CONDS:-single stress p3os}"

mkdir -p "$AE_LOG_DIR" "$OUT_DIR/results" "$OUT_DIR/figures"

WELCOME="Welcome to ChCore shell!"

ae_ensure_clean_tmux
ae_check_global_prepare || exit 1
ae_ensure_base_build || exit 1

# stress condition (traditional, single machine): source single_stress_typeN.sh
run_stress_type() {
    local type="$1"
    echo "### real: stress (traditional) type $type"
    ae_boot_cluster 1 || { ae_kill_cluster; return 1; }
    ae_send_command 0 "source single_stress_type${type}.sh"
    # Each stress script backgrounds two apps; wait long enough for both to
    # finish and flush their metric lines. TODO: replace the fixed wait with a
    # per-type completion marker once validated on a live run.
    ae_wait_in_log 0 "$WELCOME" 30 "" || true
    sleep "$(( TIMEOUT / 6 ))"
    ae_archive_logs 1 "$AE_LOG_DIR" "-stress-type${type}"
    ae_kill_cluster
}

# p3os condition (StarfishOS, two machines): source cross_stress_typeN_m{0,1}.sh
run_cross_type() {
    local type="$1"
    [ "$type" -le 4 ] || { echo "### real: no cross script for type $type (skip p3os)"; return 0; }
    echo "### real: p3os (StarfishOS cross-machine) type $type"
    ae_boot_cluster 2 || { ae_kill_cluster; return 1; }
    ae_send_command 0 "source cross_stress_type${type}_m0.sh"
    ae_send_command 1 "source cross_stress_type${type}_m1.sh"
    sleep "$(( TIMEOUT / 6 ))"
    ae_archive_logs 2 "$AE_LOG_DIR" "-p3os-type${type}"
    ae_kill_cluster
}

for cond in $CONDS; do
    case "$cond" in
        single) echo "[AE] single baselines: run each app alone (see README — per-app)."
                # TODO: single-run each application on one machine and archive
                # to <bench>_single.log. Left as a documented gap because each
                # app needs its own invocation + marker.
                ;;
        stress) for t in $STRESS_TYPES; do run_stress_type "$t" || true; done ;;
        p3os)   for t in $STRESS_TYPES; do run_cross_type "$t" || true; done ;;
    esac
done

echo ""
echo "=== Parsing + plotting real (resource-util) ==="
echo "[AE] NOTE: plot.py expects <bench>_<cond>.log; the raw logs"
echo "[AE]       above are per-stress-type and must be demuxed per application."
echo "[AE]       Until that demux is validated, re-plot from the paper CSV:"
echo "[AE]   python3 $AE_DIR/plot.py --csv $AE_REPO_ROOT/../p3os-paper/eval/real.csv --out-dir $OUT_DIR"
python3 "$AE_DIR/plot.py" --log-dir "$AE_LOG_DIR" --out-dir "$OUT_DIR" || {
    echo "[AE] log-based plot incomplete (expected until demux is validated)."
}

echo ""
echo "real figure target: $OUT_DIR/figures/real.{eps,pdf,png}"
ae_finish
