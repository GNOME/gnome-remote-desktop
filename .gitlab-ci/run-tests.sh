#!/bin/sh
set -x

pipewire &
sleep 1

wireplumber &
sleep 1

gsettings set org.gnome.desktop.remote-desktop.vnc enable true
meson test -C build --no-rebuild --verbose --no-stdsplit -t 10
