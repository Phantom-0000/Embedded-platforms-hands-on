# Embedded platforms hands on

Zephyr RTOS brightness monitoring and control exercise for the
STMicroelectronics NUCLEO-WL55JC1 board.

## Scope

The application implements three modes:

| Mode | RGB output | ADC | B2 short press | B2 long press (>= 1 s) |
| --- | --- | --- | --- | --- |
| NORMAL | Red, yellow, or green from the LDR reading | One sample immediately on entry, then every 2 s | BLUE | OFF |
| BLUE | Blue | Suspended | NORMAL | OFF |
| OFF | Off | Suspended | NORMAL | Remains OFF |

Brightness thresholds use the configured ADC full scale. Red is below 33%,
yellow is 33% through 66% inclusive, and green is above 66%. The source uses
`LDR_SENSITIVITY_PERCENT` to scale the effective brightness before applying
these required thresholds. It is currently `100` (neutral scaling); increasing
it amplifies the effective brightness for sensors with a narrow output range.

## Hardware mapping

The default `app.overlay` uses the following wiring:

| Function | STM32WL pin | Board signal |
| --- | --- | --- |
| Red output | PB11 | Nucleo LED3 / Arduino-compatible GPIO |
| Green output | PB9 | Nucleo LED2 / Arduino-compatible GPIO |
| Blue output | PB15 | Nucleo LED1 / Arduino-compatible GPIO |
| LDR analog output | PB1, ADC1_IN5 | Arduino A0 |
| B2 | PA1 | Zephyr alias `sw1` |

The three board LEDs are independent active-high outputs; there is no common
RGB LED pin to connect. PB11 is the board red LED, PB9 is green, and PB15 is
blue. If an external RGB LED is added, use one resistor per color channel and
configure its common-anode or common-cathode polarity separately. Set
`LDR_BRIGHTNESS_INVERTED` to 1 in `src/main.c` if the measured LDR voltage
decreases as light increases.

The ADC channel uses `ADC_ACQ_TIME_MAX` (160.5 STM32WL ADC cycles) because an
LDR voltage divider can have high source impedance. The longer acquisition
time lets the ADC sample-and-hold capacitor settle before the conversion.

The NUCLEO-WL55JC board manual identifies B2 as PA1. The official MB1389
schematic identifies the B2 circuit as switch B2 (KMR231GLFS), 100 nF
capacitor C69 to ground, 1 kOhm series resistor R22, and ESD protection
device U13 (ESDALC6V1-1U2). C69 and R22 form the input filtering/debouncing
network; U13 protects the input.

Sources:

- [Zephyr Nucleo WL55JC board documentation](https://docs.zephyrproject.org/4.1.0/boards/st/nucleo_wl55jc/doc/nucleo_wl55jc.html)
- [ST UM2592 NUCLEO-WL55JC user manual](https://www.st.com/resource/en/user_manual/dm00622917-stm32wl-nucleo64-board-mb1389-stmicroelectronics.pdf)
- [ST MB1389 schematic pack](https://www.st.com/resource/en/schematic_pack/mb1389-wl55jc-highband-d04_schematic.pdf)

## Thread architecture

`main.c` is the control thread. It owns the mode state, ADC sampling, RGB
updates, timing of the two-second sampling period, and serial output.

`button.c` owns one additional thread, as required by the exercise. The B2
interrupt callback writes raw press/release events to a `k_msgq`. The button
thread converts those events into exactly one short or long action and writes
the action to a second `k_msgq` consumed by `main.c`.

The Zephyr main thread keeps the default application priority. The button
thread uses priority 5 and blocks on its raw-event queue, so it runs promptly
when B2 changes but does not spin while the button is idle. The first queue
carries timestamped GPIO edges; the second carries the short/long actions
that are the only button information visible to `main.c`.

The button thread emits the long action when the one-second deadline expires,
without waiting for release. After a long action it consumes the later release
without producing a short action. The hardware debounce network is therefore
complemented by an explicit software state machine that handles event timing.

## Build and flash

This application targets Zephyr 4.1.0 and the `nucleo_wl55jc` board:

```shell
west build -b nucleo_wl55jc .
west flash
```

On Windows, keep the build checkout path free of spaces. Zephyr 4.1.0 splits
the automatically discovered overlay path at spaces, so this repository is
also available through the junction `D:\UPM\embedded_hands_on`:

```powershell
Set-Location D:\UPM\embedded_hands_on
& D:\UPM\zephyrproject\.venv\Scripts\python.exe -m west build -b nucleo_wl55jc .
Set-Location D:\UPM\zephyrproject
& D:\UPM\zephyrproject\.venv\Scripts\python.exe -m west flash `
  -d D:\UPM\embedded_hands_on\build -r openocd --skip-rebuild `
  --config D:\UPM\embedded_hands_on\openocd-nucleo-wl55jc.cfg
```

The installed environment uses Zephyr 4.1.0 from `D:\UPM\zephyrproject` and
the Zephyr SDK 0.17.0 from `D:\UPM\zephyr-sdk-0.17.0`. The user environment
variables `ZEPHYR_BASE` and `ZEPHYR_SDK_INSTALL_DIR` are set accordingly.

The project uses the installed xPack OpenOCD 0.12.0 runner. The project-local
`openocd-nucleo-wl55jc.cfg` selects the current ST-LINK/SWD adapter syntax;
Zephyr 4.1.0's board file still uses the older `hla_swd` syntax. OpenOCD is
installed at `D:\UPM\xpack-openocd-0.12.0-7` and its `bin` directory is in the
user `PATH`.

[STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html)
is an alternative STMicroelectronics flashing tool. It is not needed for the
OpenOCD command above; if the board is flashed with CubeProgrammer instead,
the installed STM32CubeProgrammer CLI 2.15.0 is at
`D:\UPM\STM32CubeProgrammer\bin` and is available on the user `PATH`; use the
`stm32cubeprogrammer` runner. The package also contains the ST-LINK/VCP driver
under `D:\UPM\STM32CubeProgrammer\Drivers\stsw-link009_v3`; its three INF
packages are preinstalled in the Windows Driver Store and will bind when the
board is connected.

The board console uses LPUART1 at 115200 baud through the ST-LINK virtual COM
port. The exact serial device name depends on the host operating system.

## ADC setup troubleshooting

If the serial console reports `brightness ADC channel setup failed (-22)`,
check that `app.overlay` uses `zephyr,reference = "ADC_REF_INTERNAL"` for
channel 5. The Zephyr 4.1.0 STM32 ADC driver rejects `ADC_REF_VDD_1` with
`-EINVAL`, even though the application compiles. `ADC_REF_INTERNAL` is the
driver's required reference setting; it does not change the LDR input to the
internal VREFINT measurement channel. The LDR remains on PB1 / ADC1_IN5.

After pulling an update, rebuild with `west build -p always -b nucleo_wl55jc .`
and flash the newly built image. Confirm that the setup error disappears and
that NORMAL prints `ADC: raw=...` approximately every two seconds. Vary the
light on the LDR and check whether the raw readings change. If the readings
change but RGB output does not, verify the RGB wiring and common-anode versus
common-cathode GPIO flags separately. Hardware validation is still required.

With neutral scaling, cover the LDR for RED, use room light near the 33%
boundary for YELLOW, and use a strong light for GREEN. If `raw` itself does
not change when the light changes, sensitivity scaling cannot compensate for a
wiring or sensor-output problem; connect the module's AO output to PB1 /
Arduino A0 and check its 3.3 V and GND.

Driver source:
[Zephyr 4.1.0 STM32 ADC](https://github.com/zephyrproject-rtos/zephyr/blob/v4.1.0/drivers/adc/adc_stm32.c).

## Expected serial output

```text
Brightness monitor started
Mode: NORMAL; ADC period: 2000 ms
ADC: raw=..., full_scale=4095, brightness=...%, color=...
Mode: BLUE; ADC acquisition suspended
Mode: OFF
System OFF
Mode: NORMAL; ADC acquisition resumed
```

## Verification checklist

- NORMAL prints one ADC result at startup and approximately every two seconds.
- Short presses switch NORMAL <-> BLUE; BLUE produces no ADC samples.
- A press held for one second enters OFF immediately from NORMAL or BLUE.
- Releasing a long press does not create a second short action.
- A short press in OFF restores NORMAL and performs an immediate ADC sample.
- Red/yellow/green decisions are checked on both sides of the 33% and 66%
  thresholds.
