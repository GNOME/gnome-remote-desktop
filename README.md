# GNOME Remote Desktop

GNOME Remote Desktop is the remote desktop server of the GNOME project. It
supports operating as a remote assistance remote desktop server, as a single
user headless remote desktop server, and as a headless remote login remote
desktop server.

It has two protocol backends, RDP and VNC. Not all modes of operation are
supported with all protocol backends.

It uses [PipeWire](https://www.pipewire.org/) for streaming pixel content,
[libei](https://gitlab.freedesktop.org/libinput/libei) for input event
plumbing, and the [Mutter remote desktop
API](https://gitlab.gnome.org/GNOME/mutter) for high level management.

For RDP support, it uses [FreeRDP](https://www.freerdp.com/), and for VNC
support, it uses [LibVNCServer](https://github.com/LibVNC/libvncserver).

It's licensed under the GNU General Public License v2 or later.

## Documentation

- [Configuration](docs/configuration.md)

## Firewall configuration

Caution is advised when considering opening up the firewall to the open
Internet. If it is necessary, here are some hints on how to achieve it.

### firewalld

To open up the firewall for connections on the default RDP port, run:

```sh
sudo firewall-cmd --permanent --add-service=rdp
sudo firewall-cmd --reload
```

To open up the firewall for connections on the default VNC port, run:

```sh
sudo firewall-cmd --permanent --add-service=vnc
sudo firewall-cmd --reload
```

## Bug reporting

Please file issues in the [issue tracker](https://gitlab.gnome.org/GNOME/gnome-remote-desktop/-/issues) on GNOME GitLab.

## Contributing

gnome-remote-desktop uses merge requests filed against the
[gnome-remote-desktop](https://gitlab.gnome.org/GNOME/gnome-remote-desktop/-/merge_requests)
GitLab module.
