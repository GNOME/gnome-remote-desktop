#!/bin/sh
set -x
pipewire &
meson test -C build --no-rebuild --verbose --no-stdsplit -t 10
