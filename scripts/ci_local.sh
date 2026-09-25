#!/usr/bin/env bash
# Local mirror of .github/workflows/ci.yml and memory-checks.yml. Run through
# `mise run ci:<step>` so the pinned tools from mise.toml are on PATH.
set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"
work="$root/build/ci-local"
mkdir -p "$work"
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
boards=(default sticky x4-pro x4-classic)  # platformio.ini default_envs
memory_boards=(default x4-pro)             # memory-checks.yml matrix

step() { printf '\n==> %s\n' "$*"; }
fail() {
  printf 'ci_local: %s\n' "$*" >&2
  exit 1
}

# CI formats a clean checkout and fails on any rewrite. Locally, compare the
# diff before and after so unrelated uncommitted edits do not count.
run_format() {
  step "clang-format"
  local before after
  before=$(git diff)
  ./bin/clang-format-fix
  after=$(git diff)
  if [[ "$before" != "$after" ]]; then
    git diff --stat
    fail "clang-format rewrote files; review and commit the formatting"
  fi
}

# CI's native jobs only receive the libraries uploaded by memory-checks.yml,
# while a local .pio has every environment's libraries. Fail on any test that
# references a library CI will not have.
check_native_deps() {
  local workflow=.github/workflows/memory-checks.yml ref env lib
  while IFS= read -r ref; do
    env=$(cut -d/ -f3 <<<"$ref")
    lib=$(cut -d/ -f4 <<<"$ref")
    [[ $env == default ]] || fail "test CMake uses $ref; CI only builds the default environment"
    grep -qE "^[[:space:]]+\.pio/libdeps/default/$lib\$" "$workflow" ||
      fail "test CMake uses $ref, which $workflow does not upload to the native jobs"
  done < <(grep -rhoE '\.pio/libdeps/[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+' test --include=CMakeLists.txt | sort -u)
}

# The QR, JPEG and PNG tests only build against the real PlatformIO sources.
ensure_codecs() {
  check_native_deps
  local libdeps=.pio/libdeps/default
  if [[ ! -f $libdeps/QRCode/src/qrcode.c || ! -f $libdeps/JPEGDEC/src/JPEGDEC.cpp ||
    ! -f $libdeps/PNGdec/src/PNGdec.cpp ]]; then
    step "Installing default PlatformIO libraries for codec tests"
    pio pkg install -e default
  fi
}

# Apple's libc++ includes headers transitively that libstdc++ does not, so a
# missing #include only fails on CI. GCC 14's libstdc++ matches CI's native jobs
# (pinned via --gcc-install-dir) and the ESP32 toolchain.
native_gcc_major=14
run_native() {
  step "Native tests with GCC $native_gcc_major/libstdc++"
  local cxx="g++-$native_gcc_major"
  if ! command -v "$cxx" >/dev/null; then
    [[ $(uname -s) == Darwin ]] && fail "$cxx not found; install it with: brew install gcc@$native_gcc_major"
    fail "$cxx not found; install g++-$native_gcc_major"
  fi
  ensure_codecs
  # GCC 15+ defaults C to C23, where the QRCode library's bool typedef is invalid.
  cmake -S test -B "$work/native-gcc" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER="${cxx/g++/gcc}" \
    -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_C_FLAGS=-std=gnu17 >/dev/null
  cmake --build "$work/native-gcc" -j "$jobs"
  ctest --test-dir "$work/native-gcc" -j "$jobs" --output-on-failure
}

# pioarduino ships an x86_64 cppcheck 2.11 even for Apple Silicon, and it
# reinstalls the whole tool package when its metadata changes. Keep its package
# and only swap in the executable from the registry's native build of the same
# 2.11 release, in both the installed package and pioarduino's source copy.
ensure_native_cppcheck() {
  [[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] || return 0
  local core="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
  if [[ ! -x "$core/packages/tool-cppcheck/cppcheck" ]]; then
    # Let pioarduino install its own package first; the Intel binary fails here.
    pio check -e "${boards[0]}" --pattern platformio.ini >/dev/null 2>&1 || true
  fi
  local native="$work/cppcheck-arm64/packages/tool-cppcheck/cppcheck"
  if [[ ! -x "$native" ]]; then
    step "Fetching native arm64 cppcheck 2.11"
    PLATFORMIO_CORE_DIR="$work/cppcheck-arm64" pio pkg install -g -t "platformio/tool-cppcheck@1.21100.241030"
  fi
  local tool
  for tool in "$core/packages/tool-cppcheck/cppcheck" "$core/tools/tool-cppcheck/cppcheck"; do
    [[ -f "$tool" ]] || continue
    file "$tool" | grep -q arm64 || cp "$native" "$tool"
  done
  "$core/packages/tool-cppcheck/cppcheck" --version | grep -qx 'Cppcheck 2.11' ||
    fail "cppcheck 2.11 is not runnable at $core/packages/tool-cppcheck"
}

run_cppcheck() {
  local scope=$1
  ensure_native_cppcheck
  local args=(--fail-on-defect low --fail-on-defect medium --fail-on-defect high)
  for board in "${boards[@]}"; do args+=(-e "$board"); done
  if [[ $scope == changed ]]; then
    local base
    base=$(git merge-base HEAD '@{upstream}' 2>/dev/null || git merge-base HEAD origin/main)
    # Default check_src_filters analyse src/ and include/ only; lib/ is not checked by CI.
    local files
    files=$( (git diff --name-only "$base" -- src include; git ls-files --others --exclude-standard -- src include) |
      grep -E '\.(c|cc|cpp|h|hpp)$' | sort -u || true)
    if [[ -z "$files" ]]; then
      step "cppcheck: no changed C/C++ sources"
      return 0
    fi
    while IFS= read -r file; do [[ -f "$file" ]] && args+=(--pattern "$file"); done <<<"$files"
  fi
  step "cppcheck ($scope)"
  pio check "${args[@]}"
}

run_sanitizers() {
  step "Native tests under ASan/UBSan"
  ensure_codecs
  # CI's options, plus the new/free pairing check that ASan leaves off on macOS
  # but enables on Linux. LeakSanitizer is unavailable on Apple Silicon.
  export ASAN_OPTIONS="alloc_dealloc_mismatch=1:halt_on_error=1"
  export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
  cmake -S test -B "$work/sanitizers" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCROSSINK_SANITIZERS=ON >/dev/null
  cmake --build "$work/sanitizers" -j "$jobs"
  ctest --test-dir "$work/sanitizers" -j "$jobs" --output-on-failure
}

run_tidy() {
  step "clang-tidy memory-lint"
  ensure_codecs
  cmake -S test -B "$work/analysis" -DCROSSINK_MEMORY_ANALYSIS=ON >/dev/null
  ctest --test-dir "$work/analysis" -R MemoryClangTidyProbe --output-on-failure
  cmake --build "$work/analysis" --target memory-lint
}

# A separate build directory keeps these instrumented builds out of .pio/build.
run_memory() {
  step "Stack gate self-test"
  cmake -DMODE=stack -DWORK="$work/probes/stack" -P test/memory_checks/checks.cmake
  for board in "${memory_boards[@]}"; do
    step "Stack and RAM budgets: $board"
    CROSSINK_MEMORY_CHECKS=1 PLATFORMIO_BUILD_DIR="$work/pio" pio run -j1 -e "$board"
    bash scripts/memory_checks/check.sh stack "$work/pio/$board" "scripts/memory_checks/stack-$board.json"
    cmake -DMODE=ram -DELF="$work/pio/$board/firmware.elf" -DENVIRONMENT="$board" \
      -DWORK="$work/probes/ram-$board" -P test/memory_checks/checks.cmake
  done
}

case "${1:-}" in
  format) run_format ;;
  native) run_native ;;
  cppcheck) run_cppcheck changed ;;
  cppcheck-all) run_cppcheck all ;;
  sanitizers) run_sanitizers ;;
  tidy) run_tidy ;;
  memory) run_memory ;;
  fast)
    run_format
    run_native
    run_cppcheck changed
    ;;
  full)
    run_format
    run_native
    run_cppcheck all
    run_sanitizers
    run_tidy
    run_memory
    ;;
  *) fail "usage: $0 format|native|cppcheck|cppcheck-all|sanitizers|tidy|memory|fast|full" ;;
esac
step "OK"
