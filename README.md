# PrimeSense OpenNI2 WinUSB Runtime

This project builds an app-local OpenNI2 PS1080 sensor plugin that uses the
Windows-provided WinUSB driver. It does not install or replace a Windows kernel
driver. It is intended for compatible PrimeSense PS1080 devices, including the
USB `VID_1D27&PID_0601` device tested with this transport.

## Requirements

- Windows 10 or later, x64
- WinUSB already assigned to the camera's sensor USB interface
- Git
- Visual Studio C++ Build Tools with the v142 toolset
- OpenNI2 for Windows x64, including its `Redist` and `Include` directories
- PowerShell

## Build the sensor plugin

From this repository:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The CMake target fetches the pinned OpenNI2 source revision and builds the
modified PS1080 plugin into `build\OpenNI2\Drivers`. Alternatively, run the
PowerShell build script directly:

```powershell
.\Build-OpenNI2-WinUSB.ps1 `
  -BuildDir .\build `
  -OutputDriverDir .\build\OpenNI2\Drivers
```

The first build downloads OpenNI2 revision
`115cf06c6efea32304182d293eca16ca883c9150`.

## Bundle the runtime with an application

Build the custom plugin into the application's `OpenNI2\Drivers` folder, then
stage the OpenNI2 redistributable runtime beside the application executable:

```powershell
.\Prepare-OpenNI2-Runtime.ps1 `
  -RedistDir 'C:\Program Files\OpenNI2\Redist' `
  -OutputDir 'C:\path\to\application'
```

The resulting application directory should contain `OpenNI2.dll` and
`OpenNI2\Drivers\PS1080.dll` plus `PS1080.ini`. Applications must use this
app-local OpenNI2 runtime; another application's bundled runtime will not
automatically use this plugin.

`primesense_winusb_probe.cpp` is an optional diagnostic utility for checking
WinUSB access and USB endpoints. It does not test the streaming protocol.

## Safety and limitations

- No driver signing policy is bypassed and no kernel driver is installed.
- This plugin changes the OpenNI2 user-mode transport only.
- USB bandwidth and camera firmware can constrain available modes.
- The depth and IR streams were validated at 640x480; color was validated at
  320x240 on the tested device.

## License

The code in this repository is licensed under the MIT License. OpenNI2 source
is downloaded at build time from its upstream repository and remains under its
own license.
