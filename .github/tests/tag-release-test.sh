#!/usr/bin/env bash
# Runs the shell of the release path from this commit against fixtures and asserts what it does:
# .github/scripts/release-tag-shape itself, the Check Release Tag step of push.yaml, and the Check Event Data step of
# build-project.yaml.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
shape="$root/.github/scripts/release-tag-shape"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
count=0
failures=0

unset RUNNER_DEBUG

# step_script <workflow file> <job> <step name>: prints the run script of that step.
step_script() {
  local script
  script=$(yq ".jobs.\"$2\".steps[] | select(.name == \"$3\") | .run" "$root/.github/workflows/$1")
  if [ -z "$script" ] || [ "$script" = "null" ]; then
    echo "the \"$3\" step was not found in $1 (job $2)" >&2
    exit 1
  fi
  printf '%s\n' "$script"
}

# expect <name> <expected> <got> [output shown on failure]
expect() {
  count=$((count + 1))
  if [ "$2" = "$3" ]; then
    echo "ok   $1"
  else
    failures=$((failures + 1))
    echo "FAIL $1"
    printf '%s\n' "$2" | sed 's/^/     expected: /'
    printf '%s\n' "$3" | sed 's/^/     got:      /'
    if [ -n "${4:-}" ]; then
      printf '%s\n' "$4" | tail -n 12 | sed 's/^/     | /'
    fi
  fi
}

# verdict <status> <output> <reason substring>: pass, fail (an ::error:: with the reason), or what else happened.
verdict() {
  if [ "$1" -eq 0 ]; then
    echo pass
  elif [[ "$2" != *"::error::"* ]]; then
    echo "abort (exit $1 without ::error::)"
  elif [[ "$2" != *"$3"* ]]; then
    echo "fail without \"$3\""
  else
    echo fail
  fi
}

# --- .github/scripts/release-tag-shape ---

# shape_case <name> <expected "exit=<status> stdout=<answer> stderr=<yes|no>"> [arguments...]
shape_case() {
  local name="$1" want="$2" out status err
  shift 2
  out=$(bash "$shape" "$@" 2> "$work/shape-err") && status=0 || status=$?
  err=no
  if [ -s "$work/shape-err" ]; then err=yes; fi
  expect "release-tag-shape: $name" "$want" "exit=${status} stdout=${out} stderr=${err}"
}
shape_case "X.Y.Z" "exit=0 stdout=false stderr=no" 1.0.3
shape_case "multi-digit X.Y.Z" "exit=0 stdout=false stderr=no" 10.20.300
shape_case "X.Y.Z-beta" "exit=0 stdout=true stderr=no" 1.0.3-beta
shape_case "X.Y.Z-betaN" "exit=0 stdout=true stderr=no" 1.0.3-beta2
shape_case "X.Y.Z-rcN" "exit=0 stdout=true stderr=no" 1.0.3-rc10
shape_case "X.Y.Z-alphaN" "exit=1 stdout= stderr=yes" 1.0.3-alpha1
shape_case "upper-case RC" "exit=1 stdout= stderr=yes" 1.0.3-RC1
shape_case "leading v" "exit=1 stdout= stderr=yes" v1.0.3
shape_case "X.Y" "exit=1 stdout= stderr=yes" 1.0
shape_case "X.Y.Z.W" "exit=1 stdout= stderr=yes" 1.0.3.4
shape_case "trailing dot" "exit=1 stdout= stderr=yes" 1.0.3.
shape_case "dotted prerelease number" "exit=1 stdout= stderr=yes" 1.0.3-rc1.2
shape_case "trailing newline" "exit=1 stdout= stderr=yes" $'1.0.3\n'
shape_case "empty argument" "exit=2 stdout= stderr=yes" ""
shape_case "no argument" "exit=2 stdout= stderr=yes"
shape_case "two arguments" "exit=2 stdout= stderr=yes" 1.0.3 1.0.4

# --- push.yaml, create-release, Check Release Tag ---

release_check=$(step_script push.yaml create-release "Check Release Tag ☑️")

# check_case <name> <ref name> <real|missing|shape check body> <expected outputs on one line, or fail>
check_case() {
  local name="$1" ref="$2" kind="$3" want="$4" dir out status got
  dir=$(mktemp -d "$work/check.XXXXXX")
  mkdir -p "$dir/source/.github/scripts"
  case "$kind" in
    real) cp "$shape" "$dir/source/.github/scripts/release-tag-shape" ;;
    missing) ;;
    *) printf '%s\n' "$kind" > "$dir/source/.github/scripts/release-tag-shape" ;;
  esac
  : > "$dir/output"
  out=$(cd "$dir" && GITHUB_REF_NAME="$ref" GITHUB_OUTPUT="$dir/output" bash -e -o pipefail -c "$release_check" 2>&1) \
    && status=0 || status=$?
  got=$(verdict "$status" "$out" "could not run")
  if [ "$got" = pass ]; then
    got=$(tr '\n' ' ' < "$dir/output")
  fi
  expect "create-release check: $name" "$want" "$got" "$out"
}
check_case "release tag" 1.0.3 real "validTag=true prerelease=false version=1.0.3 "
check_case "prerelease tag" 1.0.3-rc1 real "validTag=true prerelease=true version=1.0.3-rc1 "
check_case "tag the release path declines" test-build real "validTag=false "
check_case "missing shape check" 1.0.3 missing fail
check_case "shape check that exits 3" 1.0.3 'exit 3' fail
check_case "shape check that answers neither true nor false" 1.0.3 'echo yes' fail
check_case "shape check that answers and exits 1" 1.0.3 'echo true; exit 1' fail

# --- build-project.yaml, check-event, Check Event Data ---

event_check=$(step_script build-project.yaml check-event "Check Event Data ☑️")
# The step's ${{ }} expressions are matched as literal text and replaced with the value GitHub would substitute.
# shellcheck disable=SC2016
event_check=${event_check//'${{ github.event.number }}'/1}
# shellcheck disable=SC2016
if [[ "$event_check" == *'${{'* ]]; then
  echo "the Check Event Data step holds an expression this test does not substitute" >&2
  exit 1
fi
mkdir -p "$work/no-gh"
printf '#!/usr/bin/env bash\necho "unexpected gh call: $*" >&2\nexit 2\n' > "$work/no-gh/gh"
chmod +x "$work/no-gh/gh"

# event_outputs <event> <ref type> <ref name>: prints the build settings the step outputs, on one line.
event_outputs() {
  local dir out status
  dir=$(mktemp -d "$work/event.XXXXXX")
  cp "$root/buildspec.json" "$dir/"
  : > "$dir/output"
  out=$(cd "$dir" && PATH="$work/no-gh:$PATH" GITHUB_EVENT_NAME="$1" GITHUB_REF_TYPE="$2" GITHUB_REF_NAME="$3" \
    GITHUB_SHA=0123456789abcdef0123456789abcdef01234567 GITHUB_OUTPUT="$dir/output" GH_TOKEN=unused \
    bash -e -o pipefail -c "$event_check" 2>&1) && status=0 || status=$?
  if [ "$status" -ne 0 ]; then
    echo "exit ${status}: ${out}"
    return
  fi
  awk -F= '$1 == "codesign" || $1 == "notarize" || $1 == "package" || $1 == "config" { printf "%s ", $0 }' "$dir/output"
}
expect "check-event: push of a release tag is a packaged, notarized Release build" \
  "codesign=true notarize=true package=true config=Release " "$(event_outputs push tag 1.0.3)"
expect "check-event: workflow_dispatch on a release tag builds what a push of it builds" \
  "$(event_outputs push tag 1.0.3)" "$(event_outputs workflow_dispatch tag 1.0.3)"
expect "check-event: workflow_dispatch on a prerelease tag builds what a push of it builds" \
  "$(event_outputs push tag 1.0.3-rc1)" "$(event_outputs workflow_dispatch tag 1.0.3-rc1)"
expect "check-event: workflow_dispatch on a branch is an unpackaged RelWithDebInfo build" \
  "codesign=true notarize=false package=false config=RelWithDebInfo " "$(event_outputs workflow_dispatch branch master)"
expect "check-event: push of master is a packaged RelWithDebInfo build" \
  "codesign=true notarize=false package=true config=RelWithDebInfo " "$(event_outputs push branch master)"

echo "${count} cases, ${failures} failed"
if [ "$failures" -ne 0 ]; then
  exit 1
fi
