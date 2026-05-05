# wol

Wake-on-LAN sender for Windows, Linux, and macOS. Sends a magic packet to one or more
MAC addresses over UDP broadcast, waking machines that have WoL enabled in
their firmware.

Console application with single-file C17 implementation and no external dependencies.

C was chosen primarily as a fun exercise — I wanted a single-file, minimal-dependency
tool for a personal project. Sharing it here in case someone else finds it useful.

## Building

Pre-built binaries for Windows (x64), Linux (x86_64, aarch64), and macOS (arm64) are
published automatically via GitHub Actions when a version tag is pushed. See the
[Releases](../../releases) page.

Build scripts are included in the repo for convenient local builds and are also used
by GitHub Actions. They are intentionally kept simple and platform-specific.

For manual build commands and compiler flags, see the header comment in [`wol.c`](wol.c).

### Windows

Requires Visual Studio 2017 or later (or Build Tools for Visual Studio) with
the **Desktop development with C++** workload.

```
build.bat          # release build  →  wol.exe
build.bat debug    # debug build    →  wold.exe
```

If `cl.exe` is already on `PATH` (e.g. from a Developer Command Prompt) the
script uses it directly; otherwise it locates the toolchain automatically via
`vswhere.exe`.

### Linux

Requires a C17-capable compiler (`gcc` or `clang`) on `PATH`.

```sh
chmod +x build.sh
./build.sh          # release build  →  wol  (statically linked)
./build.sh debug    # debug build    →  wold
```

### macOS

Requires Xcode Command Line Tools (`xcode-select --install`).

```sh
chmod +x build.sh
./build.sh          # release build  →  wol
./build.sh debug    # debug build    →  wold
```

### Debug builds and self-test

Debug builds (the `debug` argument to either build script) define `WOL_SELF_TEST`,
enabling a hidden `--self-test` flag:

```
wold --self-test
```

This runs the built-in test suite and exits 0 on pass, 1 on failure. It is not
listed in `--help` and is not available in release builds.

### VSCode

A `.vscode` directory is included with build tasks and a debug launch configuration
for all three platforms. Use **Ctrl+Shift+B** to build and **F5** to build and launch the
debugger. Both invoke the platform build scripts (`build.bat` or `build.sh`). On macOS,
the debugger uses `lldb` instead of `gdb`.

## Usage

```
wol [options] <MAC> [<MAC> ...]
```

| Option | Default | Description |
|---|---|---|
| `-b, --broadcast <addr>` | `255.255.255.255` | Broadcast address: IPv4 or `IPv4/prefix` CIDR |
| `-p, --port <port>` | `9` | UDP destination port |
| `-f, --file <path>` | — | Read MAC addresses from a file |
| `--version` | — | Print version and exit |
| `-h, --help` | — | Show help text and exit |

Option flags are case-insensitive (`-B` and `-b` are equivalent).

On Windows, `/?` is also accepted as a help flag.

Use `--` to end option parsing if a MAC address starts with a `-`.

**Broadcast address:** The default `255.255.255.255` is the limited broadcast and is not forwarded by routers. For machines on a specific subnet, use the subnet-directed broadcast (e.g. `192.168.1.255`), or let `wol` compute it from any host address on the subnet using CIDR notation (e.g. `-b 192.168.1.50/24`).

**Firewall:** The UDP port must be open on the network path to the target. The target machine's own firewall is not a factor — WoL is handled by the NIC before the OS starts.

## MAC address formats

All five common formats are accepted, upper- or lowercase:

```
AA:BB:CC:DD:EE:FF
AA-BB-CC-DD-EE-FF
AA.BB.CC.DD.EE.FF
AABBCCDDEEFF
AABB.CCDD.EEFF
```

## MAC file format

One MAC address per line. Blank lines and lines where `#` is the first non-whitespace
character are ignored. Both Unix (`LF`) and Windows (`CRLF`) line endings are handled.

```
# Server room
AA:BB:CC:DD:EE:FF
11-22-33-44-55-66

# Workstations
AABBCCDDEEFF
```

## Examples

Wake a single machine on the limited broadcast:

```
wol AA:BB:CC:DD:EE:FF
```

Wake multiple machines on a specific subnet:

```
wol -b 192.168.1.255 AA:BB:CC:DD:EE:FF 11-22-33-44-55-66
```

Wake all machines listed in a file:

```
wol -f macs.txt -b 192.168.1.255
```

Mix file and command-line sources:

```
wol -f macs.txt -b 192.168.1.255 AABB.CCDD.EEFF
```

Wake machines on a subnet, letting `wol` compute the broadcast from a host address:

```
wol -b 192.168.1.50/24 AA:BB:CC:DD:EE:FF
```

## Exit codes

| Code | Meaning |
|---|---|
| `0` | All magic packets sent successfully |
| `1` | One or more packets failed, or invalid input |

## License

Copyright 2026 Jens Bråkenhielm. Licensed under the [MIT License](LICENSE).
