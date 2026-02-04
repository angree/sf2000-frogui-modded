# FrogUI v78 - Modded Fork

Custom firmware UI for SF2000 and GB300 retro gaming handhelds.

## Features

- File Manager (Total Commander style dual-panel)
- Calculator (15 decimal precision)
- Music Player with background playback
- Image/Video Viewer
- Text file viewer/editor
- Animated AVI backgrounds in themes

## Building

### Requirements

- Windows with WSL
- MIPS toolchain at /opt/mips32-mti-elf/
- sf2000_multicore framework (Desoxyn's fork): https://github.com/Trademarked69/sf2000_multicore

### Build

1. Clone sf2000_multicore somewhere on your disk
2. Edit build_sf2000.bat or build_gb300.bat - set MULTICORE_PATH to your sf2000_multicore location
3. Run the batch file

Output: core_87000000 in your multicore directory

## Contributors

- Prosty (Tomasz Zubertowski) - Original FrogUI
- Desoxyn (Trademarked69)
- Q_ta (Q_ta_s)
- angree - Modded fork

## License

CC BY-NC-SA 4.0 - See LICENSE file
