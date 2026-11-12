# Smart Lighting System

`main.c` implements the uploaded project PDF using the inspected Lab 9 LCD library.

## GP project setup

The supplied main.c needs the Lab 9 `GrLib` folder, `LcdDriver/lcd_driver.c`,
`LcdDriver/lcd_driver.h`, and `LcdDriver/middle_driver.h` in the CCS project.
Do not compile `lab9.c` or `LcdDriver/middle_driver.c` alongside this file.
The four corrected LCD HAL functions are already in main.c. In the prepared GP
copy, the original middle_driver.c is deliberately omitted.

The original Lab 9 HAL changes D/C immediately after starting a command transfer.
The replacement waits for the transfer to finish before changing D/C. Startup
also uses millisecond reset/sleep-out delays rather than the original short
cycle delays. The Lab 9 drawing library and controller settings are retained.

## Hardware-dependent assumptions

- LaunchPad: red P1.0, green P9.7, S1 P1.1, S2 P1.2; buttons active low.
- BoosterPack assumed to be BOOSTXL-EDUMKII with OPT3001 at 7-bit address 0x44.
- Sensor: SDA P4.0 and SCL P4.1, with external pull-ups to 3.3 V and common ground.
- LCD pins confirmed in Lab 9: SPI B0; clock P1.4, MOSI P1.6, CS P2.5,
  command/data P2.3, reset P9.4, backlight P2.6. Check the backlight jumper.
- LCD SPI is dedicated: the inherited driver keeps chip select active.
- DCO/MCLK is explicitly configured for nominal 8 MHz; SMCLK is divided by 8 to 1 MHz. Timing accuracy follows the
  DCO tolerance; there is no crystal calibration in this implementation.
- The sensor runs continuous 100 ms conversions. The application reads one
  completed result per second in Auto. It checks conversion-ready and overflow.

## Behavior and implementation choices

Auto is the default, with the green LED on. Four successive one-second samples
are averaged as a batch, and the resulting output is held for the next four
seconds while the next batch is collected. The exact thresholds are 100, 200,
300, and 400 lux. PWM duties are 100%, 75%, 50%, 25%, and 0% for levels 4 to 0.

Auto starts at maximum brightness until the first average. S1 changes modes.
Manual starts OFF; S2 toggles full ON/OFF, and green is off. Returning to Auto
starts a fresh four-second batch. S2 is ignored in Auto. S1 wins if both button
presses are reported together. These startup/transition choices are not defined
by the PDF and are explicitly documented in main.c.

A failed or missed reading discards the partial batch, shows an error, and
sets Auto lighting to maximum until a new complete valid batch is available.
I2C waits are bounded. Software reset cannot repair a physically stuck bus;
that condition remains an error until the wiring/device recovers.

TA0 supplies 1 kHz interrupt-driven PWM on P1.0, because the onboard red LED
pin is not a Timer_A output pin. TA1 supplies 10 ms scheduling and three-sample
button debounce. Only those two timer peripherals are used. The CPU sleeps in
LPM0 between work; SMCLK and the LCD backlight remain active. PWM interrupts
return to sleep; the scheduler wakes the foreground every 10 ms. This meets
the low-power-mode requirement but does not attempt minimum possible current.

## Verification

The application and all retained Lab 9 library sources compiled and linked
with TI MSP430 compiler 21.6.3.LTS, the FR6989 device header and the supplied
linker file, with no compiler/linker warnings or errors after the final edits.
This establishes build compatibility, not physical-board operation.

Before submitting, verify on hardware:

1. Auto/green at reset; S1 changes modes once per press and hold.
2. Manual/green off; S2 toggles fully on/off; S2 does nothing in Auto.
3. First averaged update at approximately four seconds, then every four seconds.
4. Stable lux around each threshold gives the expected level (4,3,2,1,0).
5. Red LED duty and approximately 1 kHz PWM on an oscilloscope.
6. LCD mode, live lux, average and level; Manual LED status.
7. Sensor disconnection produces an error without locking up; reconnect recovers.

## Reference material

- User's uploaded ECE370_Fall2026_Project(1).pdf.
- User's ECE370_Lab9_CCS_Project: lab9.c, GrLib and LcdDriver sources.
- TI MSP430FR6989 datasheet: https://www.ti.com/lit/ds/symlink/msp430fr6989.pdf
- TI LaunchPad guide: https://www.ti.com/lit/ug/slau627a/slau627a.pdf
- TI OPT3001 datasheet: https://www.ti.com/lit/ds/symlink/opt3001.pdf
- TI Educational BoosterPack guide: https://www.ti.com/lit/ug/slau599b/slau599b.pdf

## Additional details moved from the source header

Use the Lab 9 include paths and MSP430FR6989 linker command file. The OPT3001
address 0x44 assumes its ADDR pin is grounded. Lux arithmetic uses hundredths
of lux throughout, without floating-point math or printf. No additional timer
is used by the LCD interface.

## Sensor error diagnosis update

The persistent generic error reported on hardware has not yet been isolated.
The updated main.c now distinguishes no acknowledgment, bus busy, transfer
timeout, arbitration loss, conversion not ready, overflow, configuration
readback mismatch, and missed sample deadline on the LCD. START completion
checks follow the sequence used in Lab 7. The address and pin mapping agree
with that source. Sensor conversion time is now 100 ms to leave more margin
before each one-second read; application sampling remains once per second.
The diagnostic revision compiles and links; confirming the fault requires
reloading it onto the board and reading the new LCD message.

## Sample deadline correction

The board reported SAMPLE DEADLINE MISS, meaning foreground processing had
fallen at least one full sampling interval behind. Full LCD repainting at
1 MHz is the likely source; this has not been measured on the board.
The revision runs MCLK at 8 MHz (DCOFSEL_6, DCORSEL=0), keeps SMCLK at
1 MHz using DIVS__8, caches display lines to skip unchanged content, and
finishes the first screen before starting timers. Reset/sleep-out delay
cycle counts were adjusted for 8 MHz. Timer periods, PWM frequency, I2C
clock and UCB1BRW=64 remain the same. The diagnostic still reports genuinely
missed samples rather than accepting several readings taken together.
This revision compiled and linked without warnings/errors. Reload and
confirm four-sample averaging on hardware; build success alone does not
verify display execution time.

## Configuration validation correction

The exact mode comparison rejected continuous mode M=10 when expecting M=11.
TI defines both as continuous conversions. Validation now checks RN, CT, M1
and ME (mask 0xFC04), accepting either continuous encoding and ignoring unused
comparator settings/status flags. It still rejects shutdown, single-shot,
wrong range/conversion time and masked exponents. On a sensor error the LCD
now shows the most recently read configuration as hex for diagnosis. The
alternating configuration/restart messages are consistent with the old check
triggering retries, but the actual returned value has not yet been observed.
Regression checks for both continuous modes and all low-byte flag combinations
passed; the revision also compiled and linked successfully. Board confirmation
is still required.

## I2C transfer and identity verification revision

The observed configuration 0xFF30 is not a valid readback of 0xC610, including
when both continuous-mode encodings are accepted. Its cause is not yet proven:
configuration write/read corruption or a bus/device issue must be distinguished.
The earlier mode-mask change alone therefore did not resolve the board fault.

Transfers now use UCASTP_2 hardware automatic STOP and UCB1TBCNT, programmed
while UCSWRST is asserted, with cleared transfer flags. A register write sends
exactly three bytes. A register read sends a one-byte register selection with
STOP, then performs a separate two-byte receive with automatic final NACK/STOP.
This sequence is documented by the OPT3001 datasheet section 7.5.1; byte-counted
STOP is documented by the FR6xx family guide section 32.3.8.2. This removes the
previous manual STOP timing dependency and stale-flag assumptions.

Before writing configuration, setup reads manufacturer register 0x7E and device
register 0x7F. They must equal 0x5449 and 0x3001 respectively. An ID mismatch
shows SENSOR: ID ERROR plus the actual MFG and DEV words on the LCD, and no lux
samples are accepted. Communication errors retain their distinct messages.
The revision compiles and links successfully, but requires board verification.

References:
- https://www.ti.com/lit/ds/symlink/opt3001.pdf (7.5.1, 7.6.1.1.5, 7.6.1.1.6)
- https://www.ti.com/lit/ug/slau367p/slau367p.pdf (32.3.8.2)

## Both red LEDs follow the lighting output

At the user's request, the BoosterPack RGB red channel (J4.39 / LaunchPad
P2.6) now follows the same TA0 software PWM as the onboard red LED (P1.0).
Both receive levels 0-4 in Auto and full ON/OFF in Manual. No additional timer
is used. Equal duty does not imply equal perceived brightness between LEDs.
The modified source compiles and links successfully; board testing is pending.

Hardware setup: BoosterPack J5 must route the shared J4.39 signal to RED_LED
rather than only LCD BACKLT. The inherited LCD initialization already sets
P2.6 as a GPIO output. Once timers start, the shared signal is now PWM lighting,
not a constant-high LCD backlight signal. If J5 remains in the LCD BACKLT
position, the LCD backlight will follow the lighting output instead of the RGB
red channel. Independent constant backlighting while using RGB red may require
a separate backlight connection appropriate to the board revision. See the
BoosterPack schematic before changing wiring. These details supersede earlier
notes describing P2.6 as a permanently-on backlight output.

Reference: TI BOOSTXL-EDUMKII guide, sections 2.1.6 and 2.1.8:
https://www.ti.com/lit/ug/slau599b/slau599b.pdf
