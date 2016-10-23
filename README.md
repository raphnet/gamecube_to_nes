# Gamecube controller to NES adapter firmware

This is a project to control our good old NES with a controller that came out 15 years later...

## Features

* Connects directly to a standard NES port.
* Supports most Gamecube controllers. Tested with normal controllers, with the white japanese imports with very long cable, with the popular Nintendo Wavebird and an Intec wireless controller.
* Turbo feature. While L is pressed, A and B become turbos.
* Special mode for Mario Bros. (autorun).

## Project homepgae

Schematic and additional information are available on the project homepage:

* English: [Gamecube controller to NES adapter](http://www.raphnet.net/electronique/gc_to_nes/index_en.php)
* French: [Adaptateur manette Gamecube à NES](http://www.raphnet.net/electronique/gc_to_nes/index.php)

## Release History

* October 23, 2016 : Version 1.2
  * Updated gamecube IO code (transplanted from gcn64usb v2.9.2). Fixes some compatibility issues.

* September 2, 2013 : Version 1.1.1
  * Atmega168 support

* April 15, 2012 : Version 1.1
  * Algorithm improvement fixes Mario Bros 3.
  * Retested with all other games. (see games.txt)

* January 26, 2012 : Version 1.0.
  * Initial release

## Operation

### Button mapping

| NES   |  Gamecube |
| ------|-----------|
| A     |   A       |
| B     |   B       |
| Start  |  Start   |
| Select |  Z       |
| D-Pad  |  D-Pad or main Joystick |


### Turbo buttons

When the gamecube controller 'L' shoulder button is held
down, the A and B buttons become turbos.


### Special modes

If 'A' is held when turning on the NES, the 'auto run' mapping is
enabled. In this mode, the analog stick will press B automatically
above a certain threshold. Try playing Super Mario in this mode,
the only button you need to use is Jump!

If 'B' is held when turning on the NES, the gamecube joystick
will use a lower threshold (i.e. less deflection required to trigger
the corresponding D-Pad direction).

### Wiring

* INT0 / PD2  :  NES Latch
* PC0         :  NES Data
* PC1         :  NES Clock
* PC5         : Gamecube data (external pull up to 3.3 volt required)

The circuit is powered from the NES 5 volt. An on-board step-down regulator
is required to supply 3.3 volt to the gamecube controller.

The firmware is designed to run at 12Mhz (critical timing in the gamecube
communication code). Any other frequency will required modifications
to the code.

## License

Source code licensed under the General Public License. See gpl.txt for details.
