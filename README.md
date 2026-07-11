# ESP32-S3 HUB75 Matrix Test

PlatformIO example for driving one 64×32 HUB75 LED matrix with an
[Olimex ESP32-S3-DevKit-LiPo](https://www.olimex.com/Products/IoT/ESP32-S3/ESP32-S3-DevKit-Lipo/open-source-hardware).

## Wiring

| HUB75 signal | ESP32-S3 GPIO |
| --- | ---: |
| R1 | 4 |
| G1 | 5 |
| B1 | 6 |
| R2 | 7 |
| G2 | 15 |
| B2 | 16 |
| A | 18 |
| B | 8 |
| C | 3 |
| D | 42 |
| E | Not connected |
| LAT / STB | 40 |
| OE | 2 |
| CLK | 41 |
| GND | GND |

GPIO5 and GPIO6 share the board's optional power- and battery-sensing
circuits. Leave the corresponding solder jumpers open when using this wiring.

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

The project-local board definition in `boards/` configures the Olimex
ESP32-S3-WROOM-1-N8R8 module with 8 MB flash and 8 MB PSRAM.
