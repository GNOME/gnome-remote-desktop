# Rebuild and push with
#
#     cd .gitlab-ci/
#     podman build --format docker --no-cache -t registry.gitlab.gnome.org/jadahl/gnome-remote-desktop/master:v2 .
#     podman push registry.gitlab.gnome.org/jadahl/gnome-remote-desktop/master:v2
#

FROM fedora:32

RUN dnf -y update && dnf -y upgrade && \
    dnf install -y 'dnf-command(builddep)' && \
    dnf install -y 'pkgconfig(cairo)' && \
    dnf install -y 'pkgconfig(freerdp2)' && \
    dnf builddep -y gnome-remote-desktop && \
    dnf install -y 'pkgconfig(libvncclient)' && \
    dnf remove -y pipewire0.2-devel pipewire0.2-libs && \
    dnf install -y 'pkgconfig(libpipewire-0.3)' && \
    dnf install -y dbus-daemon xorg-x11-server-Xvfb python3-dbus python3-gobject gnome-settings-daemon mesa-dri-drivers xorg-x11-server-Xwayland mutter && \
    dnf clean all
