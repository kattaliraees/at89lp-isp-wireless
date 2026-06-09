# AT89LP6440 Test Firmware

This folder contains a simple LED blink program for the AT89LP6440 microcontroller, targeting **P1.0** (Physical Pin 1).

## Prerequisites
Ensure SDCC (Small Device C Compiler) is installed. On RedHat/Fedora-based systems, SDCC binaries are prefixed with `sdcc-`.

## Compilation

To compile the C source code and generate the Intel HEX payload, run the following commands in this directory:

```bash
# 1. Compile C source to Intel HEX format (generates main.ihx)
sdcc-sdcc main.c

# 2. Pack and format the output into the standard HEX payload
sdcc-packihx main.ihx > main.hex
```

## Files
* `main.c`: C source code.
* `main.hex`: Packed Intel HEX file ready for wireless upload via the ESP32 programmer Web UI.
