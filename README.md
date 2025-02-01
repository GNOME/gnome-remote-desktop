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

## Contents

[[_TOC_]]

## Remote assistance

The remote assistance mode provides a way to access an already active session
remotely, where both the user physically by the machine is present, and a
remote user is connecting. This means for example that locking the screen also
closes the remote desktop connection.

Running as a remote assistance remote desktop server is supported with both RDP
and VNC.

### Configuration (RDP)

#### Graphical

Open *Settings*, open the *Systems* panel then *Remote Desktop*. Select *Screen
Sharing*, enable *Desktop Sharing* and configure it for your needs.

#### From command line

1. Generate a TLS key and certificate. [See here](#tls-key-and-certificate-generation).

2. Configure GNOME Remote Desktop:

```sh
grdctl rdp set-tls-key ~/.local/share/gnome-remote-desktop/tls.key
grdctl rdp set-tls-cert ~/.local/share/gnome-remote-desktop/tls.crt
grdctl rdp set-credentials # Enter credentials via standard input
grdctl rdp enable
```

To enable remote controlling, run:

```sh
grdctl rdp disable-view-only
```

For more options, run:

```sh
grdctl --help
```

3. Enable remote assistance user service.

```sh
systemctl --user enable --now gnome-remote-desktop.service
```

### Configuration (VNC)

#### From command line

1. Configure GNOME Remote Desktop:

VNC supports two authentication methods: password or prompt. To e.g. use a
password, run:

```sh
grdctl vnc set-auth-method password
grdctl vnc set-password # Enter password via standard input
```

To enable remote controlling, run:

```sh
grdctl vnc disable-view-only
```

Then enable VNC support:

```sh
grdctl vnc enable
```

For more options, run:

```sh
grdctl --help
```

2. Enable remote assistance user service.

```sh
systemctl --user enable --now gnome-remote-desktop.service
```

## Headless multi user remote login

GNOME Remote Desktop supports integrating with the GNOME Display Manager (GDM)
to achieve remote login functionality. This feature is only available via the
RDP protocol. It works by the remote user first authenticating via a system
wide password, which gives access to the graphical login screen, where they can
login using their user specific credentials.

### Configuration

#### Graphical

Open *Settings*, open the *Systems* panel then *Remote Desktop*. Select *Remote
Login*. Unlock the panel (requires administrative privileges). Enable *Remote
Login*, and configure the remote login feature according to your needs.

#### From command line

1. Generate a TLS key and certificate. [See here](#tls-key-and-certificate-generation).

2. Configure GNOME Remote Desktop:

```sh
grdctl --system rdp set-tls-key ~gnome-remote-desktop/.local/share/gnome-remote-desktop/tls.key
grdctl --system rdp set-tls-cert ~gnome-remote-desktop/.local/share/gnome-remote-desktop/tls.crt
grdctl --system rdp set-credentials # Enter credentials via standard input
grdctl --system rdp enable
```

For more options, run:

```sh
grdctl --help
```

3. Enable system remote login service.

```sh
systemctl enable --now gnome-remote-desktop.service
```

## Headless (single user)

A single user headless remote desktop means the remote desktop client connects
directly to a GNOME Remote Desktop server running in an independently set up
headless graphical user session.

### Configuration (RDP)

1. Generate a TLS key and certificate. [See here](#tls-key-and-certificate-generation).

2. Configure GNOME Remote Desktop:

```sh
grdctl --headless rdp set-tls-key ~/.local/share/gnome-remote-desktop/tls.key
grdctl --headless rdp set-tls-cert ~/.local/share/gnome-remote-desktop/tls.crt
grdctl --headless rdp set-credentials # Enter credentials via standard input
grdctl --headless rdp enable
```

For more options, run:

```sh
grdctl --help
```

3. Enable headless single user service.

```sh
systemctl --user enable --now gnome-remote-desktop-headless.service
```

### Configuration (VNC)

1. Configure GNOME Remote Desktop:
```sh
grdctl --headless vnc set-password # Enter password via standard input
grdctl --headless vnc enable
```

2. Enable headless single user service.

```sh
systemctl --user enable --now gnome-remote-desktop-headless.service
```

## TLS key and certificate generation

Connecting via RDP requires setting up a TLS key and a TLS certificate. Here
are some examples for how to do that.

Note that for when the key and certificate is intended to be used with the
remote login system service, run each of the following commands as the
`gnome-remote-desktop` user. For example

```sh
sudo -u gnome-remote-desktop sh -c 'winpr-makecert -silent -rdp -path ~/.local/share/gnome-remote-desktop tls'
```

### FreeRDP

`winpr-makecert` is a tool from FreeRDP for generating TLS keys and
certificates for among other things RDP servers.

```sh
winpr-makecert -silent -rdp -path ~/.local/share/gnome-remote-desktop tls
```

### GnuTLS

`certtool` is an interactive tool for generating keys and certificates.

```sh
mkdir -p ~/.local/share/gnome-remote-desktop/
certtool --generate-privkey --outfile ~/.local/share/gnome-remote-desktop/tls.key
certtool --generate-self-signed --load-privkey ~/.local/share/gnome-remote-desktop/tls.key
```

### OpenSSL

`openssl` is a tool for among other things generating TLS keys and
certificates. The below example creates a certificate expiring in 720 days with
the country set to Sweden.

```sh
mkdir -p ~/.local/share/gnome-remote-desktop/
openssl req -new -newkey rsa:4096 -days 720 -nodes -x509 -subj /C=SE/ST=NONE/L=NONE/O=GNOME/CN=gnome.org -out ~/.local/share/gnome-remote-desktop/tls.crt -keyout ~/.local/share/gnome-remote-desktop/tls.key
```

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
