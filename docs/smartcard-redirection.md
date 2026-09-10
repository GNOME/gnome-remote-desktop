# Smartcard Redirection over RDP

GNOME Remote Desktop supports smartcard redirection from RDP clients. To
enable this feature, the system's OpenSC installation must be configured to
route PC/SC requests through the `gnome-remote-desktop` PC/SC provider
library, instead of the default system provider.

---

## Configuration

Edit `/etc/opensc.conf` and set `provider_library`, within the `pcsc` reader
driver section, to point to `/usr/lib64/libgrdpcsc.so.0`.

```
app default {
    reader_driver pcsc {
        provider_library = /usr/lib64/libgrdpcsc.so.0;
    }
}
```

Note: If `/etc/opensc.conf` already contains other custom options, they don't
need to change. Only `provider_library` needs to be set as shown above.

Note: The `/usr/lib64/` prefix depends on the distro's library path. For
example, on Ubuntu it's typically
`/usr/lib/x86_64-linux-gnu/libgrdpcsc.so.0`.

---

## How it works

Once the provider library is set, smartcards redirected from an RDP client
are automatically detected in the session the RDP client is connected to.

- **Session isolation**: Redirected smartcards are strictly bound to the
  active RDP session. They're not visible system-wide, so other sessions
  can't access the redirected cards.
- **Local fallback**: There's no need to maintain separate configurations for
  local and remote usage. When no RDP client is connected to a given session,
  `libgrdpcsc` falls back to reading physically connected local smartcards.
- **App compatibility**: Because the interception happens at the OpenSC
  configuration level, any application relying on OpenSC automatically
  supports redirected smartcards, without requiring application-level
  changes.
- **Login screen (GDM)**: Smartcard redirection also works at the login
  screen. This is currently only supported via `sssd`.
