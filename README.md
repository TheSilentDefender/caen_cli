# CAEN Readout (Quick Start)

Command line tool for CAEN digitizers.

It can:
- connect to one or more devices
- apply settings from a TOML config
- run acquisition and save binary waveform data
- show a live terminal view during acquisition

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

### Test a single device connection

```bash
./build/caen_cli --test --address "dig2://192.168.1.100"
```

### Read one or more FELib paths

```bash
./build/caen_cli --address "dig2://192.168.1.100" --get /par/modelname --get /par/numch
```

### Reboot device(s)

Single address:

```bash
./build/caen_cli --reboot --address "dig2://192.168.1.100"
```

From settings file addresses:

```bash
./build/caen_cli --reboot --settings example_settings.toml
```

## Output

Acquisition writes binary output files into the selected output directory using the WaveDump2 single file per board setting.

## Notes

- Press `Q` to stop acquisition.
- Press `T` to send a software trigger (when enabled by device config).
