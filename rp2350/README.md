# RP2350 version

The code for the AVR microcontroller has been ported to the RP2350 microcontroller.
RP2350 is the microcontroller used on the Raspberry Pi Pico 2 W board.

## Build instructions

Then, standing in the rp2350 subdirectory, execute:

```
mkdir build
cd build
cmake ..
make -j4
```

Copy the generated file `build/par_spi.uf2` to the microcontroller's flash.
