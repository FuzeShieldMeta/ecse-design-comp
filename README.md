# ESP32-WROOM-32 HUB75 Matrix Test

PlatformIO example for driving one 64×32 HUB75 LED matrix with a 38-pin
ESP32 development board containing an ESP32-WROOM-32 module.

## Wiring

| HUB75 signal | ESP32-WROOM-32 GPIO |
| --- | ---: |
| R1 | 25 |
| G1 | 26 |
| B1 | 27 |
| R2 | 14 |
| G2 | 13 |
| B2 | 33 |
| A | 23 |
| B | 19 |
| C | 18 |
| D | 17 |
| E | Not connected |
| LAT / STB | 32 |
| OE | 21 |
| CLK | 22 |
| GND | GND |

This mapping intentionally avoids GPIO6-GPIO11 (connected to the module's
flash), GPIO34-GPIO39 (input only), UART0 GPIO1/GPIO3, and the ESP32 boot
strapping pins. Do not substitute GPIO34-GPIO39 for any HUB75 signal: every
signal in this table is driven by the ESP32.

Power the matrix from a separate regulated 5 V supply capable of providing
the required current. Connect the matrix supply ground to the ESP32 ground.
Do not power the matrix through the ESP32 board or its USB connector.

## Build and upload

Install [PlatformIO](https://platformio.org/), connect the Olimex board, and run:

```sh
pio run
pio run --target upload
pio device monitor
```