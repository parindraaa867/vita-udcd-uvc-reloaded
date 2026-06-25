# PSVita UDCD USB Video Class plugin

## What's this?

This is a kernel plugin that lets you stream your PSVita screen to your computer via USB.

## How does it work?

The plugin uses the [SceUdcd](https://wiki.henkaku.xyz/vita/SceUdcd) module of the PSVita OS to setup
the necessary USB descriptors to simulate and behave as an [USB Video Class](https://en.wikipedia.org/wiki/USB_video_device_class) device (like a webcam or an USB video capture card).

The [hardware color space converter](https://wiki.henkaku.xyz/vita/IFTU_Registers) of the PSVita's SoC is used to perform the conversion to the destination pixel format; then the USB
controller directly performs a DMA transfer from the physical address of the resulting converted framebuffer, and therefore, saving CPU usage and power consumption.

## Supported formats and resolutions

* 960x544 @ 30 FPS and (less than) 60 FPS — native, 1:1, sharpest
* 896x504 @ 30 FPS and (almost) 60 FPS
* 864x488 @ 30 FPS and 60 FPS

## Download and installation

**Download**:

* [udcd\_uvc.skprx](https://github.com/xerpi/vita-udcd-uvc/releases)

**Compilation**

* [vitasdk](https://vitasdk.org/) is needed.

**Installation**:

1. Copy `udcd_uvc.skprx`, `udcd_uvc.txt`, and `udvc_uvc_config.vpk` to your PSVita
2. Add `udcd_uvc.skprx` to taiHEN's config (`ur0:/tai/config.txt` or `ux0:/tai/config.txt`):
```
*KERNEL
ur0:tai/udcd_uvc.skprx
```
3. Copy `udcd_uvc.txt` into `ur0:/tai/` or `ux0:/tai/` (Same directory as plugin)
4. Install `udcd_uvc_config.vpk`
5. Reboot your PSVita
6. Launch the app to change configuration settings

The build is **universal**: the same `udcd_uvc.skprx` auto-detects OLED (PCH-1000)
and LCD (PCH-2000) models, so there's no longer a separate file per model.

## Features / configuration

This fork adds:

* **Screen-off while capturing** — the Vita screen blanks when a host (e.g. OBS)
  starts capturing and comes back when it stops or you unplug. It is debounced,
  so changing resolution/FPS in OBS no longer makes it flicker.
* **Manual screen toggle** — hold **SELECT + UP** while capturing to flip the
  screen on/off by hand.
* **Keep-awake** — the console won't auto-suspend while capturing.
* **LED feedback** — the PS-button LED blips twice when a host starts capturing.
* **Config file** —  set the default resolution/FPS, screen-off behaviour, keep-awake, and boot delay
  without recompiling. See the bundled `udcd_uvc.txt` for the keys.

## Troubleshooting

If the video looks glitched, try to change the video player configuration to use the *NV12* format or switch to another player (like PotPlayer or OBS). If the colors look wrong, set color range to full and color space to BT.601 (Rec. 601).

If you use Windows 10 you might have to change the Camera access permissions on the Privacy Settings.

On Linux I recommend using *mplayer* (`mplayer tv:// -tv driver=v4l2:device=/dev/videoX:width=960:height=544`).

**Audio noise fix:**

* Disable USB power supply (Settings > System)

Note: Remember that if anything goes wrong (like PSVita not booting) you can always press L at boot to skip plugin loading.

Note 2: No, it *doesn't* stream audio. For that use a 3.5mm jack to jack adapter (a ferrite bead might help reduce the electromagnetic noise).




## Checked over with Claude
