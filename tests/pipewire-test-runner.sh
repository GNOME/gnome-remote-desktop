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
        --rdp-tls)
          enable_rdp_tls=true
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

glib-compile-schemas "$GSETTINGS_SCHEMA_DIR"

if [ "$enable_rdp" = "true" ]; then
    gsettings set org.gnome.desktop.remote-desktop.rdp enable true
    gsettings set org.gnome.desktop.remote-desktop.rdp screen-share-mode extend
fi

if [ "$enable_vnc" = "true" ]; then
    gsettings set org.gnome.desktop.remote-desktop.vnc enable true
fi

if [ "$enable_rdp_tls" = "true" ]; then
    rdp_tls_cert="$TEST_BUILDDIR/tls.crt"
    rdp_tls_key="$TEST_BUILDDIR/tls.key"

    if [ ! -f "$rdp_tls_cert" || ! -f "$rdp_tls_key" ]; then
        openssl req -new -newkey rsa:4096 -days 720 -nodes -x509 \
            -subj /C=DE/ST=NONE/L=NONE/O=GNOME/CN=gnome.org \
            -out "$rdp_tls_cert" \
            -keyout "$rdp_tls_key"
    fi
    gsettings set org.gnome.desktop.remote-desktop.rdp tls-cert "$rdp_tls_cert"
    gsettings set org.gnome.desktop.remote-desktop.rdp tls-key "$rdp_tls_key"
fi

"$@"
