#!/usr/bin/env sh

set -xe

procs=""
trap '{ kill -9 $procs 2>/dev/null || true; }' EXIT

export GSETTINGS_BACKEND="keyfile"

while [ "$#" -gt 1 ]; do
    arg="$1"
    shift
    case "$arg" in
        --enable-rdp)
          enable_rdp=true
        ;;
        --enable-vnc)
          enable_vnc=true
        ;;
        --rdp-tls-cert)
          rdp_tls_cert="$1"
          shift
        ;;
        --rdp-tls-key)
          rdp_tls_key="$1"
          shift
        ;;
        --)
          break
        ;;
        *)
          echo "Unrecoginzed argument $arg"
          exit 1
        ;;
    esac
done

pipewire=${PIPEWIRE_BIN:-pipewire}
wireplumber=${WIREPLUMBER_BIN:-wireplumber}

$pipewire &
procs="$! $procs"
sleep 1

$wireplumber &
procs="$! $procs"
sleep 1

if [ "$enable_rdp" = "true" ]; then
    gsettings set org.gnome.desktop.remote-desktop.rdp enable true
    gsettings set org.gnome.desktop.remote-desktop.rdp screen-share-mode extend
fi

if [ "$enable_vnc" = "true" ]; then
    gsettings set org.gnome.desktop.remote-desktop.vnc enable true
fi

if [ -n "$rdp_tls_cert" ]; then
    gsettings set org.gnome.desktop.remote-desktop.rdp tls-cert "$rdp_tls_cert"
fi

if [ -n "$rdp_tls_key" ]; then
    gsettings set org.gnome.desktop.remote-desktop.rdp tls-key "$rdp_tls_key"
fi

"$@"
