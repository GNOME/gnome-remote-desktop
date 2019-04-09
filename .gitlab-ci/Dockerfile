FROM fedora:30

RUN dnf -y update && dnf -y upgrade && \
    dnf install -y 'dnf-command(builddep)' && \
    dnf builddep -y gnome-remote-desktop && \
    dnf install -y 'pkgconfig(libvncclient)' && \
    dnf install -y dbus-daemon xorg-x11-server-Xvfb python3-dbus python3-gobject gnome-settings-daemon mesa-dri-drivers xorg-x11-server-Xwayland && \
    dnf install -y https://copr-be.cloud.fedoraproject.org/results/jadahl/gnome-remote-desktop/fedora-30-x86_64/00881654-mutter/mutter-3.32.0-3.fc30.x86_64.rpm && \
    dnf clean all
