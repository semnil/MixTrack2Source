#!/usr/bin/env bash
# Runs the shell of the release path from this commit against fixtures and asserts what it does:
# .github/scripts/release-tag-shape itself, the Check Release Tag step of push.yaml, the Check Event Data step of
# build-project.yaml, and the Tag and Dispatch Release step of tag-release.yaml against fixture repositories with a
# bare origin and a fake gh.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
shape="$root/.github/scripts/release-tag-shape"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
count=0
failures=0

# Fixture repositories see none of the user's or the system's git configuration.
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.invalid
export GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.invalid
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

# --- wiring between tag-release.yaml and push.yaml ---

workflows="$root/.github/workflows"
expect "wiring: tag-release.yaml runs when the workflow push.yaml names finishes" \
  "$(yq '.name' "$workflows/push.yaml")" "$(yq '.on.workflow_run.workflows | join(",")' "$workflows/tag-release.yaml")"
expect "wiring: push.yaml accepts the workflow_dispatch tag-release.yaml sends" \
  true "$(yq '.on | has("workflow_dispatch")' "$workflows/push.yaml")"
expect "wiring: push.yaml still runs on a tag pushed by hand" \
  '*' "$(yq '.on.push.tags | join(",")' "$workflows/push.yaml")"
tag_if=$(yq '.jobs.tag.if' "$workflows/tag-release.yaml")
gated=false
if [[ "$tag_if" == *"github.event.workflow_run.conclusion == 'success'"* \
  && "$tag_if" == *"github.event.workflow_run.event == 'push'"* && "$tag_if" != *"||"* ]]; then
  gated=true
fi
expect "wiring: the tag job runs only after a successful Push run started by a push" true "$gated"

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

# --- tag-release.yaml, tag, Tag and Dispatch Release ---

tag_step=$(step_script tag-release.yaml tag "Tag and Dispatch Release 🏷️")

# write_buildspec <version|absent|none|invalid>: writes buildspec.json in the fixture clone.
write_buildspec() {
  case "$1" in
    absent) rm -f "$clone/buildspec.json" ;;
    none) printf '{\n    "name": "mix-track-to-source"\n}\n' > "$clone/buildspec.json" ;;
    invalid) printf '{\n    "version": \n' > "$clone/buildspec.json" ;;
    *) printf '{\n    "name": "mix-track-to-source",\n    "version": "%s"\n}\n' "$1" > "$clone/buildspec.json" ;;
  esac
}
# new_fixture <version|absent>: a bare origin, a fake gh, and a clone whose root commit holds the shape check and
# buildspec.json.
new_fixture() {
  fixture=$(mktemp -d "$work/fixture.XXXXXX")
  origin="$fixture/origin.git"
  clone="$fixture/clone"
  git init -q --bare -b master "$origin"
  git init -q -b master "$clone"
  git -C "$clone" remote add origin "$origin"
  mkdir -p "$clone/.github/scripts" "$fixture/bin" "$fixture/runs" "$fixture/tmp"
  cp "$shape" "$clone/.github/scripts/release-tag-shape"
  : > "$fixture/gh.log"
  cat > "$fixture/bin/gh" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$*" >> "$fixture/gh.log"
if [ "\$1 \$2" = "run list" ]; then
  if [ -e "$fixture/gh-fails" ]; then echo "gh: run list failed" >&2; exit 1; fi
  shift 2
  workflow=
  branch=
  while [ \$# -gt 0 ]; do
    case "\$1" in
      --workflow) workflow=\$2; shift 2 ;;
      --branch) branch=\$2; shift 2 ;;
      *) shift ;;
    esac
  done
  if [ "\$workflow" = push.yaml ] && [ -n "\$branch" ] && [ -f "$fixture/runs/\$branch" ]; then cat "$fixture/runs/\$branch"; else echo 0; fi
  exit 0
fi
if [ "\$1 \$2" = "workflow run" ]; then exit 0; fi
echo "unexpected gh call: \$*" >&2
exit 2
EOF
  chmod +x "$fixture/bin/gh"
  commit "$1"
}
# commit <version|absent|none|invalid>: commits buildspec.json at that version with a change of its own.
commit() {
  write_buildspec "$1"
  echo "$1" >> "$clone/changes"
  git -C "$clone" add -A
  git -C "$clone" commit -q -m "version $1"
}
head_of() { git -C "$clone" rev-parse "${1:-HEAD}"; }
publish() { git -C "$clone" push -q origin master; }
remote_tag() {
  if [ "${3:-}" = annotated ]; then
    git -C "$clone" tag -a -m "$1" "$1" "$2"
  else
    git -C "$clone" tag "$1" "$2"
  fi
  git -C "$clone" push -q origin "refs/tags/$1"
}
origin_tags() {
  git -C "$origin" for-each-ref --format='%(refname:strip=2)=%(objectname)' refs/tags | LC_ALL=C sort | tr '\n' ' '
}
with_tag() { { printf '%s' "$1" | tr ' ' '\n'; echo "$2"; } | sed '/^$/d' | LC_ALL=C sort | tr '\n' ' '; }

# run_tag <name> <pass|fail> <reason substring> <expected origin tags> <expected dispatches> [tested commit]
run_tag() {
  local name="$1" want="$2" reason="$3" want_tags="$4" want_dispatch="$5" sha="${6:-}" out status got dispatched
  if [ -z "$sha" ]; then sha=$(head_of); fi
  git -C "$clone" checkout -q --detach "$sha"
  out=$(cd "$clone" && PATH="$fixture/bin:$PATH" TMPDIR="$fixture/tmp" HEAD_SHA="$sha" GH_TOKEN=unused GH_REPO=owner/repo \
    bash -e -o pipefail -c "$tag_step" 2>&1) && status=0 || status=$?
  got=$(verdict "$status" "$out" "$reason")
  dispatched=$(awk '$1 == "workflow" && $2 == "run" { printf "%s;", $0 }' "$fixture/gh.log")
  expect "tag-release: $name" "${want} | tags: ${want_tags} | dispatched: ${want_dispatch}" \
    "${got} | tags: $(origin_tags) | dispatched: ${dispatched}" "$out"
}
dispatch_of() { echo "workflow run push.yaml --ref $1;"; }

new_fixture 1.0.2
commit 1.0.3
publish
run_tag "version bump tags the tested commit and dispatches push.yaml on the tag" pass "" \
  "1.0.3=$(head_of) " "$(dispatch_of 1.0.3)"

new_fixture 1.0.2
commit 1.1.0-rc1
publish
run_tag "prerelease bump" pass "" "1.1.0-rc1=$(head_of) " "$(dispatch_of 1.1.0-rc1)"

new_fixture 1.0.2
commit 1.0.2
publish
run_tag "push that keeps the version does nothing" pass "" "" ""

new_fixture 1.0.2
publish
run_tag "root commit does nothing" pass "" "" ""

new_fixture 1.0.2
commit 1.0.3-alpha1
publish
run_tag "version the release path declines fails before tagging" fail "refusing to tag" "" ""

new_fixture 1.0.2
commit 1.0.3
publish
remote_tag 1.0.3 "$(head_of HEAD^)"
before=$(origin_tags)
run_tag "tag at another commit is left alone and fails the job" fail "already names" "$before" ""

new_fixture 1.0.2
commit 1.0.3
publish
remote_tag 1.0.3 "$(head_of HEAD^)" annotated
before=$(origin_tags)
run_tag "annotated tag at another commit is left alone and fails the job" fail "already names" "$before" ""

new_fixture 1.0.2
commit 1.0.3
publish
remote_tag 1.0.3 "$(head_of)"
before=$(origin_tags)
run_tag "tag at the tested commit with no push.yaml run on it gets the dispatch" pass "" "$before" "$(dispatch_of 1.0.3)"

new_fixture 1.0.2
commit 1.0.3
publish
remote_tag 1.0.3 "$(head_of)" annotated
before=$(origin_tags)
run_tag "annotated tag at the tested commit with no push.yaml run on it gets the dispatch" pass "" \
  "$before" "$(dispatch_of 1.0.3)"

new_fixture 1.0.2
commit 1.0.3
publish
remote_tag 1.0.3 "$(head_of)"
echo 1 > "$fixture/runs/1.0.3"
before=$(origin_tags)
run_tag "tag at the tested commit with a push.yaml run on it is not dispatched again" pass "" "$before" ""

new_fixture 1.0.2
commit 1.0.3
publish
remote_tag 1.0.3 "$(head_of)"
echo 1 > "$fixture/runs/1.0.2"
before=$(origin_tags)
run_tag "a push.yaml run on another tag does not count" pass "" "$before" "$(dispatch_of 1.0.3)"

new_fixture 1.0.2
commit 1.0.3
publish
remote_tag 1.0.30 "$(head_of HEAD^)"
run_tag "tag whose name starts with the version is another tag" pass "" \
  "$(with_tag "$(origin_tags)" "1.0.3=$(head_of)")" "$(dispatch_of 1.0.3)"

new_fixture 1.0.2
commit 1.0.3
tested=$(head_of)
commit 1.0.3
publish
run_tag "tag names the tested commit, not the tip of master" pass "" "1.0.3=${tested} " "$(dispatch_of 1.0.3)" "$tested"

new_fixture absent
commit 1.0.3
publish
run_tag "parent without buildspec.json fails before tagging" fail \
  "cannot read the version in buildspec.json at $(head_of HEAD^)" "" ""

new_fixture 1.0.2
commit none
publish
run_tag "buildspec.json without a version fails before tagging" fail \
  "cannot read the version in buildspec.json at $(head_of)" "" ""

new_fixture 1.0.2
commit invalid
publish
run_tag "buildspec.json that is not JSON fails before tagging" fail \
  "cannot read the version in buildspec.json at $(head_of)" "" ""

new_fixture 1.0.2
rm "$clone/.github/scripts/release-tag-shape"
commit 1.0.3
publish
run_tag "missing shape check fails before tagging" fail "could not run" "" ""

new_fixture 1.0.2
commit 1.0.3
publish
remote_tag 1.0.3 "$(head_of)"
touch "$fixture/gh-fails"
before=$(origin_tags)
run_tag "failing run list stops the job without a dispatch" "abort (exit 1 without ::error::)" "" "$before" ""

new_fixture 1.0.2
commit 1.0.3
publish
printf '#!/bin/sh\nexit 1\n' > "$origin/hooks/pre-receive"
chmod +x "$origin/hooks/pre-receive"
run_tag "rejected tag push stops the job without a dispatch" "abort (exit 1 without ::error::)" "" "" ""

echo "${count} cases, ${failures} failed"
if [ "$failures" -ne 0 ]; then
  exit 1
fi
