# Authentication using Kerberos

Kerberos authentication allows users to use the same mechanism to authenticate
an RDP session as they could use with for example SSH. This document describes
how to configure GNOME Remote Desktop to authenticate FreeIPA users via
Kerberos.

## Preparations

Configure the system that is going to be logged into as a FreeIPA client. See
the FreeIPA documentation for how to achieve this.

## Preparing Kerberos configuration files

Below the `<hostname>` refers to the hostname of the machine running GNOME
Remote Desktop, and `<ipa-hostname>` the hostname of the FreeIPA server.

1. Create a TERMSRV service principal.

```sh
ipa service-add TERMSRV/<hostname>
```

2. Create the Kerberos keytab file for the new TERMSRV service principal.

For system level Kerberos remote login:

```sh
sudo -u gnome-remote-desktop mkdir -m 700 -p ~gnome-remote-desktop/.local/share/gnome-remote-desktop/
ipa-getkeytab -s <ipa-hostname> -p TERMSRV/<hostname> -k ~gnome-remote-desktop/.local/share/gnome-remote-desktop/keytab.krb
chown gnome-remote-desktop ~gnome-remote-desktop/.local/share/gnome-remote-desktop/keytab.krb
```

For single user Kerberos authentication:

```sh
mkdir -p ~/.local/share/gnome-remote-desktop/
ipa-getkeytab -s <ipa-hostname> -p TERMSRV/<hostname> -k ~/.local/share/gnome-remote-desktop/keytab.krb
```

3. Configure GNOME Remote Desktop

For system level Kerberos remote login:

```sh
sudo grdctl --system rdp set-kerberos-keytab ~gnome-remote-desktop/.local/share/gnome-remote-desktop/keytab.krb
sudo grdctl --system rdp set-auth-methods kerberos
```

For single user Kerberos authentication:

```sh
grdctl --headless rdp set-kerberos-keytab ~/.local/share/gnome-remote-desktop/keytab.krb
grdctl --headless rdp set-auth-methods kerberos
```

It's possible to set the auth methods to `kerberos,credentials` to allow both
using Kerberos or configured credentials for authentication.
