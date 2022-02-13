#!/bin/sh
set -x

pipewire &
sleep 1

wireplumber &
sleep 1

openssl req -new -newkey rsa:4096 -days 720 -nodes -x509 \
            -subj "/C=DE/ST=NONE/L=NONE/O=GNOME/CN=gnome.org" \
            -keyout tls.key -out tls.crt
gsettings set org.gnome.desktop.remote-desktop.rdp tls-cert $(realpath tls.crt)
gsettings set org.gnome.desktop.remote-desktop.rdp tls-key $(realpath tls.key)
gsettings set org.gnome.desktop.remote-desktop.rdp screen-share-mode extend
gsettings set org.gnome.desktop.remote-desktop.rdp enable true

gsettings set org.gnome.desktop.remote-desktop.vnc enable true

meson test -C build --no-rebuild --verbose --no-stdsplit -t 10
