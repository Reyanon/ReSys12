# RePad

Bluetooth controllers for the PS Vita: a kernel plugin, written from scratch, MIT licensed.

- Pairs controllers the Vita does not accept by itself and reads them: **DualShock 4**, **DualSense**,
  **Switch Pro Controller** (and controllers that pair as one of these).
- Every app sees controller 1 as the Vita's own controls (port 0).
- Multi-player apps can read each controller on its own port (1-4). While an app does that, controller 1 is not
  added to port 0, so the Vita's own controls and the controller are two players.
- Apps can tell the controller's kind, and can tell RePad to stand aside or serve again without a restart (it stays
  loaded, leaving the controls as if it were not there): see `src/repad.h`.

## Install (stand-alone)

1. Copy `repad.skprx` to `ur0:tai/`.
2. Add `ur0:tai/repad.skprx` under `*KERNEL` in `ur0:tai/config.txt`.
3. Reboot, then pair the controller in Settings > Devices > Bluetooth Devices.

Do not use RePad together with another Bluetooth controller plugin. If VitaControl is loaded, RePad stays off.

The Re series emulators (ReSys12, ReST-V, ReiREM) also load RePad by themselves when it is not installed.

## Build

Needs [VitaSDK](https://vitasdk.org).

```
bash build.sh        # build/repad.skprx
```

## Notes

- Firmware 3.60-3.74: RePad hooks functions inside SceCtrl and SceBt by their offsets in those firmware versions.
- Button layout: Nintendo's face buttons by position (A = circle, B = cross, X = triangle, Y = square); L / R = L1 / R1,
  ZL / ZR = L2 / R2, - = select, + = start, HOME = PS.
- A controller that was connected before RePad started (an app that loads it) has to connect again.
