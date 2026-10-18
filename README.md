# kwai

Terminal emulator based on a serial-to-xterm bridge.

kwai connects your terminal (Windows Terminal, xterm, ...) to a serial
port. It was made for embedded systems with a cooked-input serial REPL.
Like a console app on your desktop, they take input a line at a time and don't echo it:

- **Your terminal does the typing.** The console stays in its normal line
  mode, so it echoes, edits the line and keeps a history (the up arrow, on
  Windows; on Linux and macOS, run `rlwrap kwai ...` for one). Each line is
  sent when you press Enter, ending in LF.
- **It paces pasted text.** A block pasted into the terminal goes a line
  at a time: each line waits for LiteForth's `ok>` after the one before,
  or 100 ms if no `ok>` comes. Lines typed by hand aren't held up. If a
  pasted line gives an error, the rest of the paste isn't sent.
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
such as a LiteForth CH32H417 board's, is marked `(USB CDC)`. A USB port
gets a second line with its vendor and product IDs and the strings from
its device descriptor (Manufacturer, Product and Serial Number), which
tell two boards, or a board and its debug probe, apart:

```
  COM3     \Device\Serial0
  COM15    \Device\USBSER001  (USB CDC)
           USB vvvv:pppp, manufacturer "...", product "...", serial "..."
```

Windows doesn't keep all of these strings, so kwai asks the USB hub the
device is plugged into for them, as Microsoft's USBView does (no
administrator rights needed). On Linux they come from sysfs.

On Linux it shows `/dev/serial/by-id` names next to their devices (the maker
and product, which tell boards apart), then any other `ttyACM`, `ttyUSB` or
`ttyAMA` devices, each with its USB line; on macOS, the `/dev/cu.*`
devices.

| Option | Meaning |
|---|---|
| `-b baud` | Bits per second (default 115200). A USB CDC port ignores it |
| `-p ms` | Wait up to ms for `ok>` between lines (default 100; 0 sends without waiting) |
| `-o FILE` | Add a transcript of the session to FILE (see below) |
| `--crlf` | End lines with CR LF instead of LF |

It raises DTR and RTS when it opens the port. That matters for LiteForth on
the CH32H417: its USB bridge sends nothing while DTR is off.

Lines that start with `~` are for kwai:

| Command | What it does |
|---|---|
| `~x FILE` | Types `0 ymodem` on the board, then sends FILE (Forth source) by YMODEM into the blocks its `( BLOCK n )` lines name |
| `~x FILE N` | Types `N ymodem`, then sends FILE (binary, such as a font) by YMODEM into blocks from byte 32 of block N |
| `~x FILE -1` | Types `-1 ymodem`, then sends FILE, a whole block file (`lfblocks.fb4`, plain or sealed), to replace the board's. FILE must be whole 4 KB blocks starting with `LITEFORTH`, or nothing is typed |
| `~s FILE` | Sends FILE by YMODEM to a receiver that is already waiting |
| `~f FILE` | Sends FILE's lines as if typed, each after the last one's `ok>`; stops at an error |
| `~l` | Lists the serial ports |
| `~r` | Sends Ctrl+X three times: restarts a LiteForth CH32H417 without starting its app |
| `~~TEXT` | Sends `~TEXT` |
| `~?` | Lists these, with kwai's version and build time |
| `~q` | Quits (so does Ctrl+C) |

`~x` reads FILE before it types anything on the board, so a missing file
starts nothing there.

kwai's own messages go to stderr in brackets, such as
`[kwai: COM15 lost; waiting for it to come back]`.

### Errors, `~f` and the transcript

LiteForth prompts with `ok>` even after an error, so kwai looks for the
error itself: a line ending in ` ?` (an unknown word: `foo ?`) or one
with `Error: ior=` in it, ignoring color codes. When a pasted line gives
one, kwai drops the lines of the paste still waiting to go (the ones that
came in with it); the next line you type goes as usual.

`~f FILE` sends a source file a line at a time, showing each line after
the `ok>` as if you had typed it. Once the board has shown an `ok>`, each
line waits up to 2 s for the last one's (so a line that writes flash has
time to finish); otherwise 100 ms (`-p`). It stops at the first line that
gives an error and says which, `[kwai: app.f line 12 gave an error;
stopped there]`, or ends with `[kwai: sent app.f: 40 lines]`.

`-o FILE` adds to FILE (it doesn't overwrite it) a dated header, then
everything the board sends, byte for byte, color codes included
(`less -R` shows them), each line sent, and kwai's messages. The packets of
a YMODEM transfer aren't in it, only the messages about it.

### YMODEM

Sending files is LiteForth-specific: `~x` types the `ymodem` word on the
board and sends the file with YMODEM.

- `~x core.f` types `0 ymodem` and sends Forth source into the blocks its
  `( BLOCK n )` lines name, as `putblocks.py` does.
- `~x cjk24.fnt 100` types `100 ymodem` and sends a binary file from byte
  32 of block 100, as `putbin.py` does.
- `~x lfblocks.fb4 -1` types `-1 ymodem` and sends a whole block file,
  block 0 included, which replaces the one on the board's card, so the
  card can stay in the board. It must fit in the space the card's
  `lfblocks.fb4` already has. The board checks block 0 before it writes
  anything (a sealed file must be one it can open), reads the file back
  when the transfer ends, and reports its new `capacity`.

YMODEM's first packet gives the file's name and length, so the board keeps
exactly the file's bytes, binary ones included, and none of the padding.
kwai waits up to a minute for the receiver to ask (with `C`), showing
anything the board prints meanwhile. It sends 1K packets (and a 128-byte
one for a short last piece) with CRC-16. A packet is sent up to 10 times;
after that kwai cancels the transfer with two CANs (three would be the
CH32H417's Ctrl+X x3 escape). It shows the packet count as it goes, and
LiteForth reports what it wrote when the transfer ends.

## Building

| With | Do |
|---|---|
| gcc or clang (Linux, macOS, MinGW) | `make` |
| Visual Studio 2022 | *File > Open > Folder* on this folder (it uses `CMakeLists.txt`), or `build.bat` in a Developer Command Prompt |

It is one C file, `kwai.c`, with no dependencies beyond the C library,
Win32 (with SetupAPI and CfgMgr32, for the USB strings) or POSIX threads.

## Testing

`make test` (Linux, macOS) runs `test/test_kwai.py`: LiteForth's desktop
`lf` runs on one pseudo-terminal, kwai on another, and the script bridges
them. It checks that typed lines reach `lf`, that a pasted block goes a
line per `ok>` (and 100 ms apart with the prompt off), that `~f` sends a
file and stops at an error, that a pasted line with an error drops the
rest of the paste, that `-o` keeps a transcript, that `~x` writes a file into
`lf`'s blocks byte for byte (one 2-block file with UTF-8, a 16 KB file
in 16 packets, and a binary file with Ctrl+X and SUB bytes at block 60), and that kwai reconnects when its port disappears and comes
back. It expects a LiteForth checkout next to this one; set `LF` and
`BLOCKS` to point elsewhere:

```
make test LF=/path/to/liteforth/bin/lf BLOCKS=/path/to/liteforth/bin/lfblocks.fb4
```
