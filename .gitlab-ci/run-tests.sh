#!/bin/sh
set -x

pipewire &
sleep 1

wireplumber &
sleep 1

meson test -C build --no-rebuild --verbose --no-stdsplit -t 10
