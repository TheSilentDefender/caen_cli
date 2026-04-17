# CAEN Readout (Quick Start)

Command line tool for CAEN digitizers.

It can:
- connect to one or more devices
- apply settings from a TOML config
- run acquisition and save binary waveform data
- show a live stats view during acq


## Help
```
Usage: ./build/caen_cli [OPTIONS]

Options:
  -h,--help                   Show this help message and exit
  -s,--settings,--config TEXT Settings file with address-qualified sections (required for acquisition)
  -o,--output TEXT            Output directory for generated files
  -a,--address TEXT           Device address (for --test or --get)
  -g,--get TEXT               Read and print a FELib path; may be repeated
  -t,--test                   Connect, print device info, and exit (requires --address)
  --reboot                    Send /cmd/reboot to all devices in settings file
  --gui                       Open a GUI window with latest-event plots and channel filters
  -v,--verbose                Print detailed acquisition and settings diagnostics
```
## Build

```bash
git clone git@github.com:TheSilentDefender/caen_cli.git
cd caen_cli
cmake -S . -B build
cmake --build build -j
```

## How to use

### Acquisition

```bash
./build/caen_cli --settings example_settings.toml --output ./data
```

GUI mode (separate rendering thread):

```bash
./build/caen_cli --settings example_settings.toml --output ./data --gui
```

In GUI mode, only enabled channels are listed by default. Use the per-channel checkboxes to hide/show enabled channels.

### Test a single device connection

```bash
./build/caen_cli --test --address "dig2://192.168.0.100"
```

### Read one or more FELib paths

```bash
./build/caen_cli --address "dig2://192.168.0.100" --get /par/modelname --get /par/numch
```

### Reboot device(s)

Single address:

```bash
./build/caen_cli --reboot --address "dig2://192.168.0.100"
```

From settings file addresses:

```bash
./build/caen_cli --reboot --settings example_settings.toml
```

### Configure channels with a mask

Channel settings are applied from the `["<address>".ch]` table, which sets defaults for every channel. You can override a subset of channels with a mask-based section using `["<address>".ch.mask.<mask>]`.

The mask is interpreted bitwise:
- bit 0 selects channel 0
- bit 1 selects channel 1
- bit 2 selects channel 2
- and so on

Examples:

```toml
["dig2://192.168.0.72".ch]
chenable = "True"
dcoffset = "50"
triggerthr = "3277"

["dig2://192.168.0.72".ch.mask.0x3]
triggerthr = "1000"

["dig2://192.168.0.72".ch.mask.0x10]
triggerthr = "1500"
```

In this example, the base `["...".ch]` values apply to every channel, then `mask.0x3` applies only to channels 0 and 1, and `mask.0x10` applies only to channel 4.

## Output

Acquisition writes binary output files into the selected output directory using the WaveDump2 single file per board setting.

## Notes

- Press `Q` to stop acquisition.
- Press `T` to send a software trigger when enabled.
