#!/usr/bin/env bash
# test/benchmark.sh — Run unit tests and benchmark.
# Usage: ./test/benchmark.sh [test|bench|all]

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

run_tests() {
  echo "╔══════════════════════════════════════╗"
  echo "║         UNIT TESTS                   ║"
  echo "╚══════════════════════════════════════╝"
  node --test test/unit.mjs
}

run_bench() {
  echo ""
  echo "╔══════════════════════════════════════╗"
  echo "║         BENCHMARK                    ║"
  echo "╚══════════════════════════════════════╝"
  node test/benchmark.mjs
}

case "${1:-all}" in
  test)  run_tests ;;
  bench) run_bench ;;
  all)   run_tests; run_bench ;;
  *)     echo "Usage: $0 [test|bench|all]"; exit 1 ;;
esac
