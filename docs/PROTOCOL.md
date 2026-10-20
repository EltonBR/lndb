# LNDB1 wire protocol

LNDB1 is a small framed protocol carried directly over a TCP connection. The
daemon listens on TCP port 5555 by default. Each frame begins with a 10-byte
header:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 4 | ASCII `LNDB` |
| 4 | 1 | Protocol version, `1` |
| 5 | 1 | Message type |
| 6 | 4 | Payload length, unsigned network byte order |

The payload follows immediately. Integer fields within payloads also use
network byte order. Implementations must read and write complete frames despite
TCP partial reads/writes, reject unknown versions/types and enforce payload
limits. A TCP close ends the session. Messages are sequential; one request is
active on a connection at a time.

Message type numbers are: `HELLO=1`, `AUTH=2`, `AUTH_OK=3`, `ERROR=4`,
`SHELL_REQUEST=10`, `SHELL_STDIN=11`, `SHELL_STDOUT=12`,
`SHELL_STDERR=13`, `SHELL_RESIZE=14`, `SHELL_EXIT=15`,
`FILE_REQUEST=20`, `FILE_META=21`, `FILE_DATA=22`, `FILE_END=23`,
`CLOSE=24`, `FILE_DONE=25`, `DISCOVER=30`, and `DISCOVER_REPLY=31`.
`PON_BLINK=40` requests an active-low blink sequence on GPIO15, and
`PON_BLINK_DONE=41` is reserved for completion notifications. `PON_BLINK_QUEUED=42`
acknowledges that the request was accepted into the daemon's FIFO queue, with an
empty payload.

## Session

The daemon sends an empty protocol identification (`HELLO`) frame immediately
after accepting a TCP connection. The client then sends an API key in an
authentication frame. The key is plain text. Only a successful authentication
response permits an operation request. On failure, the daemon reports
`unauthorized` and closes the connection. Clients may reconnect and try each
configured key; keys are never cached against IP addresses.

After authentication, the client requests shell, push, or pull. Shell sessions
carry terminal input/output frames; a non-interactive shell also carries
separate stdout and stderr streams and an exit status. Transfer sessions carry
path metadata and bounded binary chunks, followed by an explicit completion or
error. Directories are represented by metadata entries and files by metadata
plus byte chunks. Paths are validated by the receiver and symbolic links and
special files are rejected.

`DISCOVER` and `DISCOVER_REPLY` are reserved message identifiers in version 1
but are not exchanged by the current implementation. The `devices` command
scans IPv4 TCP endpoints and uses `HELLO` plus the normal authentication
exchange to identify LNDB daemons.

`SHELL_REQUEST` starts with one mode byte (`0` interactive PTY, `1` command,
`2` interactive script), followed by command bytes for mode `1`. For mode `2`,
the client streams the script in `SHELL_STDIN` frames and sends an empty `CLOSE`
when finished. The daemon stores at most 16 MiB in a private temporary file,
then runs `/bin/sh <temporary-file>` with a PTY; subsequent `SHELL_STDIN` and
`SHELL_RESIZE` frames control that PTY. `SHELL_RESIZE` has four unsigned
16-bit values: rows, columns, horizontal pixels, vertical pixels. `SHELL_EXIT`
and `FILE_DONE` carry one unsigned 32-bit status. A `FILE_REQUEST` starts with
direction (`1` client uploads to the daemon, `2` client downloads from it),
followed by a NUL-free path string. `FILE_META` is kind (one byte: `0` end of
tree, `1` regular file, `2` directory), mode (u32), size (u64), path length
(u16), and that many relative path bytes. Files send `FILE_DATA` chunks then an
empty `FILE_END`; directory trees end with kind `0`. `ERROR` contains a
human-readable byte string. Kind `0` must have an empty path, while file and
directory kinds must have a nonempty relative path; other kinds and embedded
NUL bytes are malformed. For both `SHELL_EXIT` and `FILE_DONE`, status zero is
success and every nonzero status is failure. The client maps a remote nonzero
status to a failing CLI exit code.

Daemon file request paths may be absolute or relative to the daemon's current
directory. Parent (`..`) components are rejected. Existing symbolic links in
any path component are rejected for both upload destinations and download
sources; regular files and directories remain usable. This is path validation,
not a filesystem sandbox: authenticated clients can still access paths
permitted by the daemon's OS credentials. The implementation checks components
with `lstat` and uses no-follow opens for final files and directories, but it
does not hold a directory-descriptor walk for the full operation. A privileged
local process that can rename path components concurrently may therefore still
create a TOCTOU race.

`PON_BLINK` is an optional, platform-specific extension. It is enabled by
default for the FiberHome AN5506-01-A custom Linux 4.1.51 build and can be
compiled out with `WITH_PON_BLINK=0`. Message identifiers stay reserved when
disabled; a daemon without the feature responds `ERROR` with
`unsupported operation`. When enabled, `PON_BLINK` has an 8-byte request
payload: `interval_ms` (u32) followed by
`times` (u32), both in network byte order. The interval must be 1..60000 ms,
the count 1..1000, and `2 * interval_ms * times` must not exceed 120000 ms.
Each ON and OFF phase lasts `interval_ms`; `times` counts complete on/off
cycles. It is accepted only after authentication. It is a platform-specific operation for the FiberHome
AN5506-01-A custom Linux 4.1.51 GPIO driver: the daemon accesses GPIO15 through
`/sys/class/gpio`, lights the active-low PON LED for each ON phase, and restores
the prior direction and value after the sequence. It unexports the line only if
it exported it. Success is
an empty `PON_BLINK_QUEUED` as soon as it is accepted into the FIFO queue. The
client exits after this acknowledgement and does not wait for execution to finish.
The daemon processes accepted blink requests in FIFO order. The queue allows 16
outstanding requests, including the one currently executing; a full queue returns
`ERROR`. GPIO or I/O failures discovered after acceptance are written to the
daemon log and cannot be returned on the completed client connection.
`PON_BLINK_DONE` is reserved and is not sent on the request connection. On daemon
shutdown, the active sequence finishes and restores GPIO state; queued sequences
are discarded. The
daemon never writes GPIO registers directly. Other Linux devices can return an
error if they do not expose this GPIO interface.

Message type values and operation payload layouts are defined in
`include/lndb_protocol.h`; that header is the wire-format source of truth.

## Security and intended network

LNDB sends credentials, commands, terminal data, and file contents without
encryption. The API key is a basic access check only. Use LNDB only on trusted,
isolated networks; this protocol does not provide confidentiality, integrity,
or protection against an on-path attacker.
