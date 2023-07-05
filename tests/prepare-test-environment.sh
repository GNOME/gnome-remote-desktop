#!/usr/bin/env sh

set -xe

tmpdir=$(mktemp -d --tmpdir g-r-d-tests-XXXXXX)
trap '{ rm -rf $tmpdir; }' EXIT

export HOME="$tmpdir/home"
export XDG_RUNTIME_DIR="$tmpdir/xrd"

mkdir -m 700 "$XDG_RUNTIME_DIR"
mkdir -m 700 "$HOME"

dbus_run_session=${DBUS_RUN_SESSION_BIN:-dbus-run-session}

$dbus_run_session -- "$@"
