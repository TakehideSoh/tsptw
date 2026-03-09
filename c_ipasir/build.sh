#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/c_ipasir"
THIRD="$WORK/third_party"
CADICAL="$THIRD/cadical"
SRC_C="$WORK/main.c"
OUT_BIN="$WORK/solver"
DEFAULT_BUNDLE_OUT="$ROOT/c_ipasir-portable.tar.gz"

if [[ ! -f "$SRC_C" ]]; then
  SRC_C="$WORK/cegar_fix_e1b3t3l1_c.c"
fi

prepare_cadical_source() {
  mkdir -p "$THIRD"

  if [[ ! -d "$CADICAL/src" ]]; then
    LOCAL_CADICAL="$(find "$ROOT/src/cegar-fix/target/release/build" -type d -path '*/out/cadical' | head -n1 || true)"
    if [[ -n "$LOCAL_CADICAL" && -d "$LOCAL_CADICAL/src" ]]; then
      echo "copying local CaDiCaL source from: $LOCAL_CADICAL"
      rm -rf "$CADICAL"
      mkdir -p "$CADICAL"
      rsync -a --delete --exclude='.git' "$LOCAL_CADICAL/" "$CADICAL/"
    else
      echo "downloading CaDiCaL 1.9.4 source..."
      rm -rf "$CADICAL" "$THIRD/cadical-rel-1.9.4" "$THIRD/cadical-rel-1.9.4.tar.gz"
      curl -fL -o "$THIRD/cadical-rel-1.9.4.tar.gz" \
        https://github.com/arminbiere/cadical/archive/refs/tags/rel-1.9.4.tar.gz
      tar -xzf "$THIRD/cadical-rel-1.9.4.tar.gz" -C "$THIRD"
      mv "$THIRD/cadical-rel-1.9.4" "$CADICAL"
    fi
  fi
}

generate_cadical_stubs() {
  # rustsat-cadical patched sources may expect these extension files in src/.
  cat > "$CADICAL/src/cadical_extension.hpp" <<'EOF'
// CaDiCaL Solver API Extension (Christoph Jabs)
// To be included in the public interface of `Solver` in `cadical.hpp`
int64_t propagations() const;
int64_t decisions() const;
int64_t conflicts() const;
#ifdef PYSAT_PROPCHECK
bool prop_check(const int *assumps, size_t assumps_len, bool psaving,
                void (*prop_cb)(void *, int), void *cb_data);
#endif
EOF
  cat > "$CADICAL/src/ccadical_extension.h" <<'EOF'
// Stub extension header for building patched CaDiCaL sources in IPASIR-only mode.
EOF
  cat > "$CADICAL/src/ccadical_extension.cpp" <<'EOF'
// Stub extension source for IPASIR-only builds.
EOF
  cat > "$CADICAL/src/solver_extension.cpp" <<'EOF'
// Stub extension source for IPASIR-only builds.
EOF
}

bundle() {
  local out="${1:-$DEFAULT_BUNDLE_OUT}"
  local tmp
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN

  prepare_cadical_source
  generate_cadical_stubs

  mkdir -p "$tmp/c_ipasir/third_party"
  if [[ -f "$WORK/main.c" ]]; then
    cp -f "$WORK/main.c" "$tmp/c_ipasir/"
  else
    cp -f "$WORK/cegar_fix_e1b3t3l1_c.c" "$tmp/c_ipasir/main.c"
  fi
  cp -f "$WORK/build.sh" "$tmp/c_ipasir/"
  cp -f "$WORK/README.md" "$tmp/c_ipasir/"
  rsync -a --exclude='.git' --exclude='build/' "$CADICAL/" "$tmp/c_ipasir/third_party/cadical/"

  tar -czf "$out" -C "$tmp" c_ipasir
  echo "created: $out"
}

build_solver() {
  prepare_cadical_source
  generate_cadical_stubs

  local need_build=0
  if [[ ! -f "$CADICAL/build/libcadical.a" ]]; then
    need_build=1
  elif ! nm -g "$CADICAL/build/libcadical.a" 2>/dev/null | grep "_*ipasir_init" >/dev/null; then
    echo "existing libcadical.a missing ipasir_init (wrong arch?), rebuilding..."
    rm -f "$CADICAL/build/libcadical.a"
    need_build=1
  fi

  if [[ "$need_build" -eq 1 ]]; then
    echo "building CaDiCaL with IPASIR..."
    local jobs
    jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    (cd "$CADICAL" && ./configure && make -j"$jobs")
  fi

  if ! nm -g "$CADICAL/build/libcadical.a" | grep "_*ipasir_init" >/dev/null; then
    echo "error: built libcadical.a does not export ipasir_init" >&2
    exit 1
  fi

  echo "building C solver..."
  local static_flag=""
  if [[ "$(uname)" = "Linux" ]]; then
    static_flag="-static"
  fi
  gcc -O2 -std=c11 -Wall -Wextra -pedantic \
    $static_flag \
    -D_POSIX_C_SOURCE=200809L \
    "$SRC_C" \
    -I"$CADICAL/src" \
    "$CADICAL/build/libcadical.a" \
    -lstdc++ -lm \
    -o "$OUT_BIN"

  echo "built: $OUT_BIN"
}

build_tsptw() {
  prepare_cadical_source
  generate_cadical_stubs

  local need_build=0
  if [[ ! -f "$CADICAL/build/libcadical.a" ]]; then
    need_build=1
  elif ! nm -g "$CADICAL/build/libcadical.a" 2>/dev/null | grep "_*ipasir_init" >/dev/null; then
    rm -f "$CADICAL/build/libcadical.a"
    need_build=1
  fi
  if [[ "$need_build" -eq 1 ]]; then
    local jobs
    jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    (cd "$CADICAL" && ./configure && make -j"$jobs")
  fi

  echo "building TSPTW solver..."
  local static_flag=""
  if [[ "$(uname)" = "Linux" ]]; then
    static_flag="-static"
  fi
  gcc -O2 -std=c11 -Wall -Wextra -pedantic \
    $static_flag \
    -D_POSIX_C_SOURCE=200809L \
    "$WORK/tsptw_main.c" \
    -I"$CADICAL/src" \
    "$CADICAL/build/libcadical.a" \
    -lstdc++ -lm \
    -o "$WORK/tsptw_solver"

  echo "built: $WORK/tsptw_solver"
}

usage() {
  cat <<'EOF'
usage:
  ./c_ipasir/build.sh                 # build solver (default)
  ./c_ipasir/build.sh build           # build solver
  ./c_ipasir/build.sh bundle [OUT]    # create portable tar.gz
  ./c_ipasir/build.sh all [OUT]       # build solver + portable tar.gz
  ./c_ipasir/build.sh tsptw          # build TSPTW solver
EOF
}

cmd="${1:-build}"
case "$cmd" in
  build)
    build_solver
    ;;
  bundle)
    bundle "${2:-$DEFAULT_BUNDLE_OUT}"
    ;;
  all)
    build_solver
    bundle "${2:-$DEFAULT_BUNDLE_OUT}"
    ;;
  -h|--help|help)
    usage
    ;;
  tsptw)
    build_tsptw
    ;;
  *)
    usage
    exit 2
    ;;
esac
