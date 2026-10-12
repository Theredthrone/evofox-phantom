# evofox-phantom

## CLI LED control for Evofox / gaming mice

### Features
- Control LED modes: slow, fast, static, off
- Send custom raw HID reports (requires `--beta` flag)
- Filter devices by VID/PID or name
- List available devices
- Works with Evofox Phantom and compatible mice

### Building
```bash
cc -O2 -o evofox-phantom evofox-phantom.c
```

### Usage
```bash
# Set LED mode (no --beta needed)
./evofox-phantom slow
./evofox-phantom static
./evofox-phantom fast
./evofox-phantom off

# Send raw HID report (requires --beta flag)
./evofox-phantom --beta --raw 07:13:7f:16:0f:00:00:00

# List devices
./evofox-phantom --list

# Filter by VID/PID
./evofox-phantom --vid 0x18f8 --pid 0x...
```

### Payload format
Raw payloads are space-separated hex bytes (e.g., `07:13:7f:16:0f:00:00:00`).
The first two bytes (`07:13`) are the report ID and length.
The remaining bytes control the LED behavior.

### Payload testing
Use `probe.sh` to send payloads one at a time and log descriptions:
```bash
./probe.sh
```
Results are logged to `colur`.