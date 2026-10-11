#!/bin/sh
# Runs real programs under the malloc shim and checks that (a) they behave
# normally and (b) the shim actually served their allocations.
#   tests/shim_test.sh [path-to-shim]
set -eu
case "$(uname -s)" in
  Darwin) SHIM=${1:-build/libmalloc_shim.dylib}; PRELOAD=DYLD_INSERT_LIBRARIES ;;
  *)      SHIM=${1:-build/libmalloc_shim.so};     PRELOAD=LD_PRELOAD ;;
esac
SHIM=$(cd "$(dirname "$SHIM")" && pwd)/$(basename "$SHIM")
fail=0

# run_under <expected-stdout-regex|-> cmd...
run_under() {
  expect=$1; shift
  out=$(env "$PRELOAD=$SHIM" LM_SHIM_REPORT=1 "$@" 2>build/shim_stderr.txt) || {
    echo "FAIL (exit status): $*"; cat build/shim_stderr.txt; fail=1; return; }
  if [ "$expect" != "-" ] && ! printf '%s\n' "$out" | grep -Eq "$expect"; then
    echo "FAIL (output mismatch): $*"; fail=1; return; fi
  rep=$(grep 'libmalloc-shim' build/shim_stderr.txt || true)
  m=$(printf '%s' "$rep" | sed -n 's/.*malloc=\([0-9]*\).*/\1/p')
  if [ -z "$m" ] || [ "$m" -eq 0 ]; then
    echo "FAIL (shim not in use): $*"; fail=1; return; fi
  echo "ok: $*  ->  $rep"
}

mkdir -p build
cc -O1 -std=gnu11 -o build/shim_probe tests/shim_probe.c
run_under 'probe ok' build/shim_probe

if [ "$(uname -s)" = Linux ]; then
  run_under 'total' ls -la /usr
  run_under '^499999500000$' python3 -c "print(sum(range(10**6)))"
  run_under '^ok$' sh -c 'echo ok | sort | uniq'
else
  # SIP-protected system binaries strip DYLD_INSERT_LIBRARIES; use any
  # non-system python3 if one exists.
  PY=""
  for c in /opt/homebrew/bin/python3 /usr/local/bin/python3; do
    [ -x "$c" ] && PY=$c && break
  done
  if [ -n "$PY" ]; then
    run_under '^499999500000$' "$PY" -c "print(sum(range(10**6)))"
  else
    echo "skip: no non-system python3 on this macOS box"
  fi
fi
exit $fail
