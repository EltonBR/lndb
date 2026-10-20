#!/usr/bin/env python3
"""Install LNDB on the custom AN5506-01-A image through FTP and Telnet."""

import ftplib
import ipaddress
import os
import re
import socket
import struct
import sys
import time
import uuid


LOGIN_PROMPT = re.compile(rb"login\s*:", re.IGNORECASE)
PASSWORD_PROMPT = re.compile(rb"password\s*:", re.IGNORECASE)
LOGIN_ERROR = re.compile(rb"login incorrect|authentication failed", re.IGNORECASE)
SHELL_PROMPT = re.compile(rb"(?:^|[\r\n])[a-zA-Z0-9_.()/-]*[#$>]\s*$")
IAC = 255
WILL, WONT, DO, DONT, SB, SE = 251, 252, 253, 254, 250, 240


class InstallError(Exception):
    pass


class TelnetShell:
    def __init__(self, host, timeout=12):
        self.sock = socket.create_connection((host, 23), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buffer = bytearray()
        self.state = 0
        self.command = 0

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _send_negotiation(self, command, option):
        try:
            self.sock.sendall(bytes((IAC, command, option)))
        except OSError:
            pass

    def _decode_telnet(self, raw):
        clean = bytearray()
        for byte in raw:
            if self.state == 0:
                if byte == IAC:
                    self.state = 1
                else:
                    clean.append(byte)
            elif self.state == 1:
                if byte == IAC:
                    clean.append(IAC)
                    self.state = 0
                elif byte in (WILL, WONT, DO, DONT):
                    self.command = byte
                    self.state = 2
                elif byte == SB:
                    self.state = 3
                else:
                    self.state = 0
            elif self.state == 2:
                if self.command == WILL:
                    self._send_negotiation(DONT, byte)
                elif self.command == DO:
                    self._send_negotiation(WONT, byte)
                self.state = 0
            elif self.state == 3:
                if byte == IAC:
                    self.state = 4
            elif self.state == 4:
                self.state = 0 if byte == SE else 3
        return clean

    def expect(self, patterns, timeout=12):
        deadline = time.monotonic() + timeout
        while True:
            for pattern in patterns:
                match = pattern.search(self.buffer)
                if match:
                    result = bytes(self.buffer[:match.end()])
                    del self.buffer[:match.end()]
                    return pattern, result
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise InstallError("timeout waiting for the ONU Telnet prompt")
            self.sock.settimeout(remaining)
            try:
                raw = self.sock.recv(4096)
            except (OSError, socket.timeout) as exc:
                raise InstallError("Telnet connection failed while waiting for a prompt") from exc
            if not raw:
                raise InstallError("Telnet connection closed by the ONU")
            self.buffer.extend(self._decode_telnet(raw))

    def sendline(self, text):
        self.sock.sendall(text.encode("ascii") + b"\r\n")

    def login_root(self):
        self.expect([LOGIN_PROMPT], timeout=15)
        self.sendline("root")
        prompt, _ = self.expect([PASSWORD_PROMPT, LOGIN_ERROR, SHELL_PROMPT], timeout=10)
        if prompt is LOGIN_ERROR:
            raise InstallError("Telnet rejected the root login")
        if prompt is PASSWORD_PROMPT:
            self.sendline("")
            prompt, _ = self.expect([LOGIN_ERROR, SHELL_PROMPT], timeout=10)
            if prompt is LOGIN_ERROR:
                raise InstallError("Telnet rejected the empty root password")
        # Prove that the login prompt has handed control to a shell.
        self.run(":", timeout=5)

    def run(self, command, timeout=15):
        token = "__LNDB_RC_{}__".format(uuid.uuid4().hex)
        line = "{}; __lndb_rc=$?; printf '\\n{}:%s\\n' \"$__lndb_rc\"".format(
            command, token
        )
        self.sendline(line)
        pattern = re.compile(re.escape(token.encode("ascii")) + rb":([0-9]+)\r?\n")
        deadline = time.monotonic() + timeout
        while True:
            match = pattern.search(self.buffer)
            if match:
                output = bytes(self.buffer[:match.start()])
                status = int(match.group(1))
                del self.buffer[:match.end()]
                return status, output
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise InstallError("timeout while running a remote installation command")
            self.sock.settimeout(remaining)
            try:
                raw = self.sock.recv(4096)
            except (OSError, socket.timeout) as exc:
                raise InstallError("Telnet command did not complete") from exc
            if not raw:
                raise InstallError("Telnet session ended during an installation command")
            self.buffer.extend(self._decode_telnet(raw))


def require_success(telnet, command, description, timeout=20):
    status, _ = telnet.run(command, timeout=timeout)
    if status != 0:
        raise InstallError(description)


def verify_elf(path):
    try:
        with open(path, "rb") as binary:
            header = binary.read(20)
    except OSError as exc:
        raise InstallError("cannot read the MIPS daemon binary") from exc
    if (len(header) < 20 or header[:4] != b"\x7fELF" or header[4] != 1 or
            header[5] != 2 or header[18:20] != b"\x00\x08"):
        raise InstallError("daemon must be a 32-bit big-endian MIPS ELF")


def upload_ftp(host, local_path, remote_path):
    ftp = ftplib.FTP()
    try:
        ftp.connect(host, 21, timeout=20)
        ftp.login("root", "")
        ftp.set_pasv(True)
        with open(local_path, "rb") as binary:
            ftp.storbinary("STOR " + remote_path, binary, blocksize=65536)
        ftp.quit()
    except ftplib.all_errors + (OSError,) as exc:
        try:
            ftp.close()
        except OSError:
            pass
        raise InstallError("FTP upload failed; verify root FTP access on the ONU") from exc


def wait_for_lndb(host, port=5555, timeout=12):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1) as sock:
                sock.settimeout(1)
                header = bytearray()
                while len(header) < 10:
                    chunk = sock.recv(10 - len(header))
                    if not chunk:
                        raise OSError("short LNDB greeting")
                    header.extend(chunk)
                if (header[:4] == b"LNDB" and header[4] == 1 and
                        header[5] == 1 and struct.unpack("!I", header[6:10])[0] == 0):
                    return
                raise OSError("port 5555 did not return an LNDB1 greeting")
        except OSError as exc:
            last_error = exc
            time.sleep(0.25)
    raise InstallError("new daemon did not become ready on TCP/5555") from last_error


def configured_port(telnet):
    command = (
        "port=$(awk 'BEGIN { s=0 } /^\\[/ { s=($0==\"[server]\"); next } "
        "s && /^[[:space:]]*port[[:space:]]*=/ { print $3; exit }' "
        "/etc/lndbd/lndbd.conf); "
        "[ -n \"$port\" ] || port=5555; printf '__LNDB_PORT__:%s\\n' \"$port\""
    )
    status, output = telnet.run(command, timeout=10)
    match = re.search(rb"__LNDB_PORT__:([0-9]+)", output)
    if status != 0 or not match:
        raise InstallError("could not read the daemon port from its configuration")
    port = int(match.group(1))
    if port < 1 or port > 65535:
        raise InstallError("daemon configuration contains an invalid TCP port")
    return port


def install(host, binary, version):
    host = str(ipaddress.IPv4Address(host))
    binary = os.path.abspath(binary)
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise InstallError("invalid project VERSION")
    verify_elf(binary)

    suffix = uuid.uuid4().hex[:12]
    stage = "/opt/lndb/.lndbd.upload." + suffix
    telnet = TelnetShell(host)
    ftp_uploaded = False
    try:
        telnet.login_root()
        preflight = (
            '[ "$(uname -r)" = "4.1.51" ] && '
            '[ "$(uname -m)" = "mips" ] && '
            "grep -qi BCM68385 /proc/cpuinfo && "
            "grep -q ' /opt ' /proc/mounts && "
            "test -x /start-on-boot.sh && test -w /start-on-boot.sh && "
            "test -d /opt && test -w /opt"
        )
        require_success(
            telnet, preflight,
            "target rejected: expected writable custom BCM68385 Linux 4.1.51 MIPS image",
        )
        require_success(telnet, "mkdir -p /opt/lndb && test -w /opt/lndb",
                        "cannot prepare /opt/lndb on the ONU")

        print("Uploading lndbd {} to {} via FTP...".format(version, host), flush=True)
        ftp_uploaded = True
        upload_ftp(host, binary, stage)

        status, output = telnet.run(
            "chmod 0755 {} && {} --version".format(stage, stage), timeout=20
        )
        if status != 0 or ("lndbd " + version).encode("ascii") not in output:
            raise InstallError("uploaded daemon failed its on-device version check")

        startup_command = "/opt/lndb/lndbd --config /etc/lndbd/lndbd.conf"
        status, _ = telnet.run(
            "grep -Fq {} /start-on-boot.sh".format("'" + startup_command + "'"),
            timeout=10,
        )
        if status != 0:
            lines = [
                "# LNDB startup managed by install-onu.sh",
                "if [ -x /opt/lndb/lndbd ]; then",
                "    /opt/lndb/lndbd --config /etc/lndbd/lndbd.conf >> /var/log/lndbd.log 2>&1 &",
                "fi",
            ]
            printf_args = " ".join("'{}'".format(line.replace("'", "'\\''")) for line in lines)
            register = (
                "if [ ! -e /start-on-boot.sh.pre-lndb ]; then "
                "cp -p /start-on-boot.sh /start-on-boot.sh.pre-lndb || exit; fi && "
                "printf '%s\\n' {} >> /start-on-boot.sh && "
                "chmod 0755 /start-on-boot.sh"
            ).format(printf_args)
            require_success(telnet, register, "could not register LNDB in start-on-boot.sh")
            print("Registered LNDB in /start-on-boot.sh (backup: .pre-lndb).", flush=True)
        else:
            print("LNDB startup entry already exists; leaving it unchanged.", flush=True)

        promote = (
            "if [ -x /opt/lndb/lndbd ] && [ ! -e /opt/lndb/lndbd.previous ]; then "
            "cp -p /opt/lndb/lndbd /opt/lndb/lndbd.previous || exit; fi && "
            "mv {} /opt/lndb/lndbd"
        ).format(stage)
        require_success(telnet, promote, "could not install the new daemon")
        ftp_uploaded = False

        restart = r'''pid=
for p in /proc/[0-9]*; do
    [ -r "$p/cmdline" ] || continue
    cmd=$(tr '\000' ' ' < "$p/cmdline")
    case "$cmd" in
        "/opt/lndb/lndbd --config /etc/lndbd/lndbd.conf"*)
            ppid=$(sed -n 's/^PPid:[[:space:]]*//p' "$p/status")
            if [ "$ppid" = 1 ]; then pid=${p##*/}; fi
            ;;
    esac
done
if [ -n "$pid" ]; then
    kill -TERM "$pid" || exit
    i=0
    while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 10 ]; do
        sleep 1
        i=$((i + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then exit 1; fi
fi
nohup /opt/lndb/lndbd --config /etc/lndbd/lndbd.conf >> /var/log/lndbd.log 2>&1 < /dev/null &
newpid=$!
sleep 1
kill -0 "$newpid" 2>/dev/null || exit 1
printf '__LNDB_STARTED__:%s\n' "$newpid"'''
        status, output = telnet.run(restart, timeout=18)
        if status != 0 or b"__LNDB_STARTED__:" not in output:
            raise InstallError("could not restart lndbd; previous binary is saved as lndbd.previous")

        port = configured_port(telnet)
        wait_for_lndb(host, port)
        print("Installed and started lndbd {} on {}:{}.".format(version, host, port))
        print("The current API key/configuration was preserved; no firmware or flash was changed.")
    finally:
        if ftp_uploaded:
            try:
                ftp = ftplib.FTP()
                ftp.connect(host, 21, timeout=5)
                ftp.login("root", "")
                ftp.delete(stage)
                ftp.quit()
            except ftplib.all_errors + (OSError,):
                pass
        telnet.close()


def main(argv):
    if len(argv) != 4:
        print("usage: install-onu.sh ONU_IPV4", file=sys.stderr)
        return 2
    try:
        install(argv[1], argv[2], argv[3])
    except (InstallError, ipaddress.AddressValueError) as exc:
        print("install-onu: {}".format(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
