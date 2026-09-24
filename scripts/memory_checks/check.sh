#!/usr/bin/env bash
# Thin adapters for GCC reports, binutils and clang-tidy. No source edits/rebaselining.
set -euo pipefail
export LC_ALL=C
here=$(cd "$(dirname "$0")" && pwd -P)
root=$(cd "$here/../.." && pwd -P)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail() { printf '%s\n' "$*" >&2; exit 1; }
command -v jq >/dev/null || fail 'jq is required'
mode=${1:?Usage: check.sh stack|ram|tidy ...}
shift
case "$mode" in
stack)
  build=${1:?build directory}; budget=${2:?stack baseline}; root=${3:-$root}
  root=$(cd "$root" && pwd -P)
  manifest="$build/memory-stack-manifest.json"
  jq -e 'type == "object" and length > 0' "$manifest" >/dev/null
  jq -er '.sources[]' "$budget" | sort -u > "$work/required"
  jq -er '.[]' "$manifest" | sort -u > "$work/sources"
  comm -23 "$work/required" "$work/sources" > "$work/missing"
  while IFS= read -r source; do
    [[ ! -f "$root/$source" ]] || fail "Missing stack instrumentation: $source"
  done < "$work/missing"
  jq -er 'keys[]' "$manifest" > "$work/reports"
  : > "$work/reports.su"
  while IFS= read -r report; do
    case "$report" in /*|../*|*/../*) fail "Invalid stack report: $report";; esac
    [[ "$report" == *.su && -f "$build/$report" ]] || fail "Missing stack report: $report"
    cat "$build/$report" >> "$work/reports.su"
  done < "$work/reports"
  jq -r '.exceptions | to_entries[] | [.key,.value] | @tsv' "$budget" > "$work/limits"
  limit=$(jq -er '.limit | select(type == "number")' "$budget")
  awk -F '\t' -v root="$root/" -v limit="$limit" '
    FILENAME == ARGV[1] { budget[$1]=$2; next }
    {
      if (NF != 3 || $2 !~ /^[0-9]+$/ || $3 !~ /^(static|dynamic|dynamic,bounded)$/ ||
          $1 !~ /:[0-9]+:[0-9]+:/) { print "Malformed stack report: " $0; bad=1; next }
      key=$1; sub(/:[0-9]+:[0-9]+:/, ":", key)
      if (index(key,root)==1) key=substr(key,length(root)+1)
      if (key !~ /^(src|lib)\// || key ~ /^lib\/(uzlib\/|miniz\/third_party\/|EpdFont\/builtinFonts\/|Epub\/Epub\/hyphenation\/generated\/)/) next
      count++; cap=(budget[key]>limit ? budget[key] : limit)
      if ($3=="dynamic") { print key ": unbounded stack"; bad=1 }
      else if ($2>cap) { print key ": " $2 " exceeds " cap " bytes"; bad=1 }
    }
    END { if (!count) { print "Missing project stack records"; bad=1 }
          print "Checked " count " stack records"; exit bad }
  ' "$work/limits" "$work/reports.su"
  ;;
ram)
  elf=${1:?firmware ELF}; environment=${2:?environment}; budget=${3:-$here/static-ram-budgets.json}
  packages="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages"
  case "$environment" in
    default) reader="$packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-readelf"; machine=RISC-V;;
    x4-pro) reader="$packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-readelf"; machine='Tensilica Xtensa';;
    *) fail "Unknown environment: $environment";;
  esac
  "$reader" -h -SW "$elf" > "$work/elf"
  jq -er --arg env "$environment" '.[$env].limits | to_entries[] | [.key,.value] | @tsv' "$budget" > "$work/limits"
  awk -v machine="$machine" '
    function hex(s, n,i) { n=0; for(i=1;i<=length(s);i++) n=n*16+index("0123456789abcdef",tolower(substr(s,i,1)))-1; return n }
    FILENAME == ARGV[1] { limit[$1]=$2; next }
    /Type:/ { executable=($2=="EXEC") }
    /Machine:/ { architecture=(index($0,machine)>0) }
    /^ *\[ *[0-9]+\] +\./ {
      sub(/^ *\[ *[0-9]+\] +/, "")
      name=$1; bytes=hex($5); flags=$7
      if (!bytes || flags !~ /A/ || name ~ /^\.flash[._]/ || name==".dram0.dummy" || name==".ext_ram.dummy") next
      group=""
      if(name ~ /^\.iram/) group="iram"
      else if(name ~ /^\.dram/ || name==".noinit") group="dram"
      else if(name ~ /^\.rtc/) group="rtc"
      else if(flags ~ /W/) { print "Unclassified writable section: " name; bad=1 }
      if(group!="") total[group]+=bytes
    }
    END {
      if(!executable || !architecture || !total["dram"] || !total["iram"]) { print "Wrong/incomplete firmware ELF"; bad=1 }
      for(group in total) {
        print group " RAM bytes: " total[group] " / " limit[group]
        if(!(group in limit) || total[group]>limit[group]) { print group " exceeds budget"; bad=1 }
      }
      exit bad
    }
  ' "$work/limits" "$work/elf"
  ;;
tidy)
  build=${1:?native build directory}; tool=${2:-clang-tidy}
  baseline="$here/clang-tidy-baseline.json"
  "$tool" --version | grep -F 'LLVM version 21.1.6' >/dev/null || fail 'clang-tidy 21.1.6 is required'
  checks='-*,bugprone-sizeof-expression,bugprone-implicit-widening-of-multiplication-result,bugprone-suspicious-stringview-data-usage,clang-analyzer-core.NullDereference,clang-analyzer-core.StackAddressEscape,clang-analyzer-unix.Malloc,clang-analyzer-cplusplus.NewDelete,clang-analyzer-cplusplus.NewDeleteLeaks'
  extra=()
  if [[ $(uname -s) == Darwin ]]; then
    sdk=$(xcrun --show-sdk-path)
    extra=("--extra-arg=-isysroot" "--extra-arg=$sdk" "--extra-arg=-isystem" "--extra-arg=$sdk/usr/include/c++/v1")
  fi
  jq -er --arg root "$root/" '[.[].file | select(startswith($root)) | ltrimstr($root) | select(test("^(src|lib)/") and (test("^lib/(uzlib/|miniz/third_party/|EpdFont/builtinFonts/|Epub/Epub/hyphenation/generated/)")|not))] | unique[]' "$build/compile_commands.json" > "$work/sources"
  [[ -s "$work/sources" ]] || fail 'Missing production compile commands'
  jq -er '.sources[]' "$baseline" | sort -u > "$work/required"
  comm -23 "$work/required" "$work/sources" > "$work/missing"
  [[ ! -s "$work/missing" ]] || { cat "$work/missing"; fail 'Missing analysis coverage'; }
  pids=(); n=0; failed=0
  while IFS= read -r source; do
    n=$((n+1))
    "$tool" "$root/$source" -p "$build" --quiet --config='{}' --header-filter='.*' --checks="$checks" "${extra[@]}" > "$work/$n.log" 2>&1 &
    pids+=("$!")
    if [[ ${#pids[@]} == 4 ]]; then
      for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
      pids=()
    fi
  done < "$work/sources"
  if (( ${#pids[@]} )); then
    for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
  fi
  cat "$work/"*.log > "$work/tidy.log"
  [[ $failed == 0 ]] || { cat "$work/tidy.log"; fail 'clang-tidy failed'; }
  # Diagnostic locations deduplicate header findings across compile variants.
  awk '/:[0-9]+:[0-9]+: warning: .* \[(bugprone-|clang-analyzer-)[^]]+\]$/ {
    path=$0; sub(/:[0-9]+:[0-9]+: warning:.*/,"",path)
    loc=substr($0,length(path)+2); split(loc,a,":")
    check=$0; sub(/^.* \[/,"",check); sub(/\]$/,"",check)
    print path "\t" a[1] "\t" a[2] "\t" check
  }' "$work/tidy.log" | sort -u > "$work/diagnostics"
  : > "$work/canonical"
  while IFS=$'\t' read -r path line column check; do
    path="$(cd "$(dirname "$path")" && pwd -P)/$(basename "$path")"
    relative=${path#"$root/"}
    case "$relative" in lib/uzlib/*|lib/miniz/third_party/*|lib/EpdFont/builtinFonts/*|lib/Epub/Epub/hyphenation/generated/*) continue;; esac
    case "$relative" in src/*|lib/*|include/*) ;; *) continue;; esac
    printf '%s\t%s\t%s\t%s\n' "$relative" "$line" "$column" "$check" >> "$work/canonical"
  done < "$work/diagnostics"
  sort -u "$work/canonical" > "$work/unique"
  : > "$work/findings"
  while IFS=$'\t' read -r relative line column check; do
    path="$root/$relative"
    text=$(sed -n "${line}p" "$path" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    printf '%s|%s|%s\n' "$relative" "$check" "$text" >> "$work/findings"
  done < "$work/unique"
  jq -Rn '[inputs] | group_by(.) | map({key:.[0],value:length}) | from_entries' < "$work/findings" > "$work/counts"
  jq -nr --slurpfile current "$work/counts" --slurpfile baseline "$baseline" '$current[0] | to_entries[] | select(.value > ($baseline[0].findings[.key] // 0)) | "\(.key): \(.value) new/increased findings"' > "$work/new"
  [[ ! -s "$work/new" ]] || { cat "$work/new"; fail 'New clang-tidy diagnostics'; }
  printf 'clang-tidy: %s sources; %s existing findings\n' "$n" "$(wc -l < "$work/findings" | tr -d ' ')"
  ;;
*) fail "Unknown check: $mode";;
esac
