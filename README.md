# kwai

Terminal emulator based on a serial-to-xterm bridge.

kwai connects your terminal (Windows Terminal, xterm, ...) to a serial
port. It was made for embedded systems with a cooked-input serial REPL.
Like a console app on your desktop, they take input a line at a time and don't echo it:

- **Your terminal does the typing.** The console stays in its normal line
  mode, so it echoes, edits the line and keeps a history (the up arrow, on
  Windows; on Linux and macOS, run `rlwrap kwai ...` for one). Each line is
  sent when you press Enter, ending in LF.
- **It shows what the board sends** as it arrives, UTF-8 included (CJK
  works), and passes escape sequences on to the terminal, which handles
  colors and cursor moves.
- **It reconnects by itself.** When the port goes away, as a USB CDC port
  does while the board is reflashed, kwai says so and reopens it as soon as
  it comes back. It also waits for a port that isn't there yet.

## Using it

```
kwai COM15                  Windows
rlwrap kwai /dev/ttyACM0    Linux, with rlwrap for line history
kwai -b 9600 /dev/ttyUSB0   a UART at 9600 baud
kwai -l                     list the serial ports
```

`kwai -l` (or `kwai` with no port) lists the ports there are. On Windows
it shows each COM port with the driver device behind it; a USB CDC port,
such as a LiteForth CH32H417 board's, is marked `(USB CDC)`:

```
  COM3     \Device\Serial0
  COM15    \Device\USBSER000  (USB CDC)
```

On Linux it shows `/dev/serial/by-id` names next to their devices (the maker
and product, which tell boards apart), then any other `ttyACM`, `ttyUSB` or
`ttyAMA` devices; on macOS, the `/dev/cu.*` devices.

| Option | Meaning |
|---|---|
| `-b baud` | Bits per second (default 115200). A USB CDC port ignores it |
| `--crlf` | End lines with CR LF instead of LF |

It raises DTR and RTS when it opens the port. That matters for LiteForth on
the CH32H417: its USB bridge sends nothing while DTR is off.

Lines that start with `~` are for kwai:

| Command | What it does |
|---|---|
| `~x FILE` | Types `xmodem` on the board, then sends FILE by XMODEM |
| `~s FILE` | Sends FILE by XMODEM to a receiver that is already waiting |
| `~l` | Lists the serial ports |
| `~r` | Sends Ctrl+X three times: restarts a LiteForth CH32H417 without starting its app |
| `~~TEXT` | Sends `~TEXT` |
| `~?` | Lists these |
| `~q` | Quits (so does Ctrl+C) |

kwai's own messages go to stderr in brackets, such as
`[kwai: COM15 lost; waiting for it to come back]`.

### XMODEM

The XMODEM send feature is LiteForth-specific. It triggers LiteForth into XMODEM mode
and sends the file using the `~x` command.

- `~x core.f` types `xmodem` on the board and sends the file into blocks.

kwai waits up to a minute for the receiver to start, showing anything the
board prints meanwhile. It sends 1K packets (and a 128-byte one for a short
last piece), with an 8-bit checksum or CRC-16, whichever the receiver asks
for: LiteForth's `xmodem` asks for checksums. A packet is sent up to 10
times; after that kwai cancels the transfer. It shows the packet count as
it goes.

## Building

| With | Do |
|---|---|
| gcc or clang (Linux, macOS, MinGW) | `make` |
| Visual Studio 2022 | *File > Open > Folder* on this folder (it uses `CMakeLists.txt`), or `build.bat` in a Developer Command Prompt |

It is one C file, `kwai.c`, with no dependencies beyond the C library,
Win32 or POSIX threads.

## Testing

`make test` (Linux, macOS) runs `test/test_kwai.py`: LiteForth's desktop
`lf` runs on one pseudo-terminal, kwai on another, and the script bridges
them. It checks that typed lines reach `lf`, that `~x` writes a file into
`lf`'s blocks byte for byte (one 2-block file with UTF-8, and a 16 KB file
in 16 packets), and that kwai reconnects when its port disappears and comes
back. It expects a LiteForth checkout next to this one; set `LF` and
`BLOCKS` to point elsewhere:

```
make test LF=/path/to/liteforth/bin/lf BLOCKS=/path/to/liteforth/bin/lfblocks.fb4
```
