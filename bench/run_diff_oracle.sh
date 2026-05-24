#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="$REPO_ROOT/build-wsl"
JOURNAL="/mnt/c/Users/bhask/Documents/PROJECTS/market-data-handler/bench/results/itch_1m_ab_source.journal"
SIGNAL_FILE="$REPO_ROOT/examples/filtered_momentum.sig"
OUT_ROOT="$REPO_ROOT/bench/results/diff_test"
RUN_ID="diff_oracle"
MAX_EVENTS=0
MIN_WITHIN_RATE=0.999
FIXTURE=""
EVENTS_CSV_INPUT=""
USE_EVENTS_BACKTEST=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --journal)
      JOURNAL="$2"
      shift 2
      ;;
    --signal-file)
      SIGNAL_FILE="$2"
      shift 2
      ;;
    --out-root)
      OUT_ROOT="$2"
      shift 2
      ;;
    --events-csv)
      EVENTS_CSV_INPUT="$2"
      shift 2
      ;;
    --fixture)
      FIXTURE="$2"
      shift 2
      ;;
    --run-id)
      RUN_ID="$2"
      shift 2
      ;;
    --max-events)
      MAX_EVENTS="$2"
      shift 2
      ;;
    --min-within-rate)
      MIN_WITHIN_RATE="$2"
      shift 2
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

BACKTEST_RUNNER="$BUILD_DIR/backtest_runner"
JOURNAL_TO_CSV="$BUILD_DIR/journal_to_csv"
EVENTS_CSV="$OUT_ROOT/journal_events.csv"
JIT_CSV="$OUT_ROOT/jit_signals.csv"
REF_CSV="$OUT_ROOT/reference_signals.csv"
REPORT_JSON="$OUT_ROOT/divergence_report.json"
REPORT_MD="$OUT_ROOT/divergence_report.md"

if [[ ! -x "$BACKTEST_RUNNER" ]]; then
  echo "ERROR: missing backtest_runner at $BACKTEST_RUNNER" >&2
  exit 1
fi
if [[ -z "$EVENTS_CSV_INPUT" && -z "$FIXTURE" && ! -x "$JOURNAL_TO_CSV" ]]; then
  echo "ERROR: missing journal_to_csv at $JOURNAL_TO_CSV" >&2
  exit 1
fi

mkdir -p "$OUT_ROOT"

if [[ -n "$FIXTURE" ]]; then
  if [[ "$FIXTURE" != "synthetic_aapl" ]]; then
    echo "ERROR: unknown fixture: $FIXTURE" >&2
    exit 2
  fi
  python3 "$REPO_ROOT/test/reference_oracle/generate_fixture_events.py" --out-csv "$EVENTS_CSV"
  EVENTS_CSV_INPUT="$EVENTS_CSV"
  USE_EVENTS_BACKTEST=1
elif [[ -n "$EVENTS_CSV_INPUT" ]]; then
  if [[ "$(realpath "$EVENTS_CSV_INPUT")" != "$(realpath -m "$EVENTS_CSV")" ]]; then
    cp -f "$EVENTS_CSV_INPUT" "$EVENTS_CSV"
  fi
  EVENTS_CSV_INPUT="$EVENTS_CSV"
  USE_EVENTS_BACKTEST=1
else
  CSV_ARGS=(--journal "$JOURNAL" --out-csv "$EVENTS_CSV")
  if [[ "$MAX_EVENTS" != "0" ]]; then
    CSV_ARGS+=(--max-events "$MAX_EVENTS")
  fi
  "$JOURNAL_TO_CSV" "${CSV_ARGS[@]}"
fi

if [[ "$USE_EVENTS_BACKTEST" == "1" ]]; then
  BACKTEST_ARGS=(--events-csv "$EVENTS_CSV_INPUT" --signal-file "$SIGNAL_FILE" --out-root "$OUT_ROOT" --run-id "$RUN_ID")
else
  BACKTEST_ARGS=(--journal "$JOURNAL" --signal-file "$SIGNAL_FILE" --out-root "$OUT_ROOT" --run-id "$RUN_ID")
fi

if [[ "$MAX_EVENTS" != "0" ]]; then
  BACKTEST_ARGS+=(--max-events "$MAX_EVENTS")
fi

"$BACKTEST_RUNNER" "${BACKTEST_ARGS[@]}"
cp -f "$OUT_ROOT/$RUN_ID/signals.csv" "$JIT_CSV"

python3 "$REPO_ROOT/test/reference_oracle/compute_reference.py" \
  --events-csv "$EVENTS_CSV" \
  --signal-file "$SIGNAL_FILE" \
  --out-csv "$REF_CSV"

python3 "$REPO_ROOT/test/reference_oracle/diff_signals.py" \
  --jit-csv "$JIT_CSV" \
  --ref-csv "$REF_CSV" \
  --out-json "$REPORT_JSON" \
  --out-md "$REPORT_MD" \
  --min-within-rate "$MIN_WITHIN_RATE"

echo "jit_csv=$JIT_CSV"
echo "reference_csv=$REF_CSV"
echo "report_json=$REPORT_JSON"
echo "report_md=$REPORT_MD"
