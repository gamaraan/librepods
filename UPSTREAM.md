# This directory is a modified copy of librepods

`daemon/` is the AirPods daemon the bar widget needs. It is **not** original
work: it is a modified copy of [librepods](https://github.com/kavishdevar/librepods)
by **Kavish Devar**, who reverse-engineered Apple AAP over L2CAP and the BLE
advertisement path that carries battery, in-ear and case lid state. That is the
hard part of this project, and it is his.

| | |
|---|---|
| Upstream | https://github.com/kavishdevar/librepods |
| Forked at | `29a914c`, 2026-05-19 |
| Licence | GNU General Public License v3.0, see `LICENSE` in this directory |
| Modified by | GM, through 2026-08-16 |

The copy here is the `linux/` subtree only, minus its `extras/` developer
helpers, because that is all the widget needs. Upstream also ships an Android
app and a root module. None of that is reproduced here; get it from upstream.

## What was modified

130 commits of Linux daemon work sit on top of the fork point. The changes that
matter to this widget:

- **A published state file.** The daemon writes its whole status as one line of
  JSON to `$XDG_STATE_HOME/librepods/status.json` when the state changes, and
  removes it on quit. This is what lets the panel watch a file and run no
  processes at all while idle.
- **A `status` verb and control verbs** on the local socket: `ca:`, `onebud:`,
  `adaptive:N`, `ear:`, alongside the upstream noise verbs.
- **AirPods Pro 3 support**, including the `A3064` model map and a
  `supports_noise_off` flag, because the Pro 3 has no Off listening mode and
  silently ignores the packet.
- **Case lid state** reported from the BLE advertisement.
- **The control socket moved off `/tmp`** to `$XDG_RUNTIME_DIR/librepods.sock`,
  which is mode 0700, and both binaries refuse to fall back.
- **A systemd user unit**, bound to `graphical-session.target`.
- **Notifications through the host desktop** rather than a Qt tray toast.
- **A `--headless` mode**, which the systemd unit runs. It builds neither the
  tray nor the QML engine, so most of Qt Gui, Widgets, Qml and Quick is never
  paged in. Measured on the box: 46 MB resident against 61 MB for the same
  binary under `--hide`, of which 13 MB stays resident either way because one
  binary still links those libraries and their initialisers run regardless.
  Toasts moved to a `Notifier` so they survive without a tray, and `--hide` is
  untouched and can still open the window later, which headless cannot.
- **Memory and reliability work**: the QML engine load is deferred under
  `--hide`, and the daemon no longer grabs the pods on every local playback.

## Building it

```bash
cd daemon
cmake -B build -G Ninja
cmake --build build
cmake --install build --prefix ~/.local
```

Needs `cmake`, `ninja`, `qt6-connectivity`, `qt6-tools`, `qt6-declarative` and
`libpulse` from the Arch `extra` repository, and `pkgconf` from `core`. With that
prefix the install step puts `librepods.service` in `~/.local/share/systemd/user`,
which systemd searches, so `systemctl --user enable --now librepods.service` finds it
by name.

## Licence

This directory is GPL-3.0, inherited from upstream, and stays GPL-3.0. The bar
widget in the repository root is a separate program that talks to this daemon
over a state file and a command line, and is MIT. See the repository README.
