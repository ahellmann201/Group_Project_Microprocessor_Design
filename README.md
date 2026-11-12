# Group Project: Microprocessor Design

ECE370 Smart Lighting System for the MSP430FR6989 LaunchPad and Educational
BoosterPack Mark II, developed in Code Composer Studio.

## Features

- Auto mode reads the OPT3001 light sensor once per second and averages four
  samples. The result sets LED brightness for the next four-second period.
- Both red LEDs follow brightness levels 4 through 0 (100%, 75%, 50%, 25%, OFF)
  at average lux thresholds of 100, 200, 300 and 400.
- LaunchPad S1 changes Auto/Manual mode; S2 toggles both red LEDs in Manual.
- The LaunchPad green LED indicates Auto mode. The LCD shows mode and readings.
- Uses LPM0 and two Timer_A peripherals, with bounded I2C transfers and sensor
  identity/configuration checks.

## Open and build

Import this folder as an existing CCS project, select the MSP430FR6989 target,
and build. The source includes the required Lab 9 graphics and LCD library.
The four low-level LCD interface functions are supplied by `main.c`; do not add
the original `middle_driver.c` or `lab9.c` alongside it.

Use TI MSP430 compiler 21.6.3.LTS or a compatible version. The project settings
reserve 256 bytes for the stack. Build outputs and local backups are ignored.

## Hardware setup

- LaunchPad red: P1.0; green: P9.7; S1: P1.1; S2: P1.2.
- OPT3001: I2C address 0x44, SDA P4.0, SCL P4.1.
- LCD connections follow the included Lab 9 driver.
- BoosterPack RGB red: P2.6 / J4.39. Select **RED_LED** at jumper **J5**.
  This pin is shared with LCD backlight control; the LCD backlight follows the
  PWM signal if the jumper is left selecting LCD BACKLT instead.

See [BUILD_NOTES.md](BUILD_NOTES.md) for hardware assumptions, implementation
choices, diagnostic history and verification steps. The user confirmed the
sensor worked after the byte-counted I2C revision. The latest dual-red-LED
revision compiled and linked; physical validation of that addition is pending.

## Contents

- `main.c`: application and corrected low-level LCD interface.
- `GrLib/`: TI graphics library and fonts.
- `LcdDriver/`: LCD controller driver and interface headers.
- `.ccsproject`, `.cproject`, `.project`, `.settings/`, `targetConfigs/`:
  CCS project and device settings.
- `lnk_msp430fr6989.cmd`: device linker command file.

TI-derived source files retain their original license notices.
