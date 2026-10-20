# LNDB — Linux Network Debug Bridge

LNDB provides a small C11 client (`lndb`) and device-side daemon (`lndbd`) for
remote shell access and file transfer over TCP. It uses POSIX/Linux interfaces
and has no required third-party libraries.

`VERSION` is the persistent version counter, and the Makefile generates the
header used by both executables. Each product build invocation (`make`,
`make client`, `make daemon`, or `make install`) increments the patch number
once, including builds started through `build.sh`. The client and daemon built
in the same invocation report the same version. `make version` prints the
counter without changing it; `make test` and `make clean` do not increment it.
Check a built binary with `lndb --version` or `lndbd --version`.

For implementation structure and code responsibilities, see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). For a step-by-step setup and
command guide, see [docs/USAGE.md](docs/USAGE.md). The wire format is in
[docs/PROTOCOL.md](docs/PROTOCOL.md); FiberHome-specific platform facts are in
[FIBERHOME-AN5506-01-A-PLATFORM.md](FIBERHOME-AN5506-01-A-PLATFORM.md).

## Build

On Debian or Ubuntu, prepare the host and the MIPS SDK in
`$HOME/.toolchains/mips32--musl--stable-2018.02-2` with:

```sh
./install-deps.sh
```

This installs host compiler/build tools and the runtime libraries required by
the SDK, then relocates it to the toolchain directory when needed. Set
`TOOLCHAIN_DIR` to use another external SDK location. The ONU runtime is
supplied by its existing musl-based firmware; this script does not install
anything on the device.

```sh
make                 # build/lndb and build/lndbd
make client          # build/lndb
make daemon          # build/lndbd
make version         # print current version counter without incrementing
make test             # local protocol, client, transfer and daemon regressions
make test-sanitize    # native ASan + UBSan regression run
make clean
```

Install the client and daemon with `make install`. By default, `lndb` is
installed in `$HOME/.local/bin` and `lndbd` in `$HOME/.local/sbin`. The client
keeps its mutable configuration in
`${XDG_CONFIG_HOME:-~/.config}/lndb/lndb.conf`, with mode `0600`, so adding
devices never requires root or writes API keys under `/etc`. Override the
prefix or stage an installation with `DESTDIR`:

```sh
make install PREFIX=/usr/local
make install DESTDIR=/tmp/package-root
```

`BINDIR` and `SBINDIR` can also be overridden independently.
The install target does not invoke `sudo`; use writable destinations or run it
with the needed privileges.

For the MIPS32 big-endian toolchain outside the repository, use the convenience
script. It links the ONU binaries statically by default:

```sh
./build.sh             # build-mips/lndb and build-mips/lndbd
./build.sh daemon      # daemon only
./build.sh client      # client only
```

To deploy the daemon to one custom AN5506-01-A by IPv4 address, run
`./install-onu.sh 192.168.10.29`. It builds the MIPS daemon if needed, uploads
it over FTP, updates the startup hook over Telnet, and restarts LNDB without
rebooting or writing firmware/MTD. The installer refuses targets that do not
match the custom BCM68385/MIPS/Linux 4.1.51 runtime. It requires host Python 3
and the FTP/Telnet access configured by that image.

Set `JOBS`, `BUILD_DIR`, or `CC` to override its defaults. Other make variables
such as `CFLAGS`, `CPPFLAGS`, `LDFLAGS`, and `LDLIBS` are passed through.

Cross compile by selecting the compiler and a separate build directory, for
example:

```sh
make CC="$HOME/.toolchains/mips32--musl--stable-2018.02-2/bin/mips-buildroot-linux-musl-gcc" BUILD_DIR=build-mips
```

The cross-compiled daemon is placed at `build-mips/lndbd`, separate from the
host binaries under `build/`.

The daemon defaults to foreground mode and listens on `0.0.0.0:5555`. On its
first run it creates `/etc/lndbd/lndbd.conf` with a random 256-bit API key and
mode `0600`. Register that key from the client with
`lndb add device HOST API_KEY`; the client validates it and saves it in the
client configuration. Use `lndbd --config PATH` to select another config file.

See [Configuration](docs/CONFIGURATION.md) for both INI formats, defaults,
accepted values, permissions, and the discovery ID map.

The client creates a private `lndb.conf` in
`${XDG_CONFIG_HOME:-~/.config}/lndb/` on first use, independent of the current
working directory. It has mode `0600` and starts with an empty key list. Added
API keys remain in this per-user file. You can select another file with
`--config PATH`. Example:

```ini
[client]
port = 5555
connect_timeout_ms = 500

[discovery]
network = 192.168.10.*
concurrency = 32

[auth]
api_key_list = key-from-device-one,key-from-device-two
```

To start with no registered devices, leave the value empty (`api_key_list =`)
and use `lndb add device` to validate and register the first key.

`[client] port` is the default port for discovery and device commands. A
per-device command can override it with `HOST:PORT`, for example
`lndb 192.168.10.29:8989 shell`. IPv6 endpoints with an explicit port use
brackets, such as `lndb '[fe80::1]:8989' shell`.

`network` also accepts IPv4 CIDR notation and inclusive address ranges such as
`192.168.10.2-192.168.10.30`. The `devices` command reports authenticated LNDB
daemons as `device` and identified daemons for which none of the configured
keys work as `unauthorized`. Each listed service gets a numeric ID (`1`, `2`, ...)
in ascending IP order. `devices` saves the latest ID-to-IP-and-port mapping in
`lndb.conf.devices` beside the client config; IDs refer to that last scan and
are refreshed by running `lndb devices` again.

## Commands

```sh
lndb --version
lndbd --version
lndb devices
lndb add device 192.168.10.29 <API_KEY>
lndb --config ./lndb.conf add device 192.168.10.29:8989 <API_KEY>
lndb 192.168.10.10 shell
lndb 1 shell
lndb 192.168.10.10 shell uname -a
lndb 192.168.10.10 shell "ps | grep init"
lndb 192.168.10.29 blink 100 10
lndb 1 push ./arquivo /tmp/
lndb 1 pull /tmp/logfile.log .
lndb 192.168.10.10 pull /tmp/logfile.log .
lndb 192.168.10.10 pull /opt/projeto ./
lndb 192.168.10.10 push ./arquivo /tmp/
lndb 192.168.10.10 push ./projeto /opt/
```

`blink INTERVAL_MS TIMES` is a platform extension for the AN5506-01-A custom
Linux 4.1.51 image. It toggles the PON LED using active-low GPIO15. For example,
`blink 100 10` performs 10 complete on/off cycles,
with each phase lasting 100 ms, then restores its previous GPIO direction and
level. The client exits after the daemon confirms the request was queued; it
does not wait for the sequence to finish. Requests are processed in FIFO order.
The queue holds 16 outstanding requests, including the active sequence. A full
queue is reported immediately; GPIO errors discovered after acceptance go to
the daemon log. On shutdown, the active sequence completes and restores GPIO
state while queued requests are discarded. Interval is limited to 1..60000 ms,
count to 1..1000, and total time to 120 seconds.

PON blink support is enabled by default. Build without the platform-specific
CLI command, daemon queue, and GPIO implementation using `WITH_PON_BLINK=0`;
use a separate build directory when changing this option:

```sh
make BUILD_DIR=build-no-blink WITH_PON_BLINK=0
```

For a MIPS daemon without blink support, run
`WITH_PON_BLINK=0 BUILD_DIR=build-mips-no-blink ./build.sh daemon`. LNDB1
message type numbers remain reserved, and a daemon built without this feature
returns `unsupported operation` for blink requests.

`add device` validates the supplied 64-character hexadecimal API key against
the specified LNDB daemon. Only after successful authentication does it append
the key to `[auth] api_key_list` in the selected client config (or the active
per-user config). The config update is atomic and sets mode `0600`; rejected
keys are never saved. If the key is already listed, the command validates it
and leaves the config unchanged. The active client config is private to the
user and is never written under `/etc`.

With command arguments, the client joins the arguments with spaces and passes
the resulting text to `/bin/sh -c` remotely. Shell metacharacters are therefore
interpreted by the remote shell; quote commands as appropriate. With redirected
stdin, `lndb HOST shell < script.sh` streams the script and displays its output.
If the client has a controlling terminal, it runs the script under a remote PTY
and forwards user input too, so script prompts can be answered. Without a
controlling terminal, it runs the script non-interactively and forwards stdin
through EOF. `lndb HOST shell sh -s < script.sh` explicitly selects the latter
mode. A final newline is optional: the remote shell executes the last line when
it reaches EOF. A bare `ls` can appear to produce no output when the remote
working directory is empty or contains only hidden files; use `ls -la` to show
those entries. Without a command or redirection, LNDB allocates a remote PTY
and forwards the local terminal, including window size changes.

For transfers, a directory copied to an existing directory is placed beneath
that directory using its source basename; when the destination does not exist,
it becomes the copied directory. Directory trees are recursive. Symlinks and
special files are rejected.

## Security

LNDB1 transmits the API key, commands, terminal data, and files without
encryption. Its API key is only a basic access control mechanism. Use it on
trusted, isolated networks; do not expose the service to untrusted networks.
The project does not configure or install the daemon on the ONU, and does not
modify firmware, kernel, bootloader, or flash partitions.

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the wire protocol.
