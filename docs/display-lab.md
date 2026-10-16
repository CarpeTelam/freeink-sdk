# Display lab

The display lab is a bench API for measuring a panel: it drives a
caller-supplied waveform with caller-supplied raw image planes, one activation
at a time, and reports how long the controller held BUSY. It exists so a
firmware can calibrate frame periods, drive polarity and per-pixel grayscale
on the device itself, on the UltraChip X4 Pro batches as well as on SSD1677.

It is compiled only with `-DFREEINK_CAP_DISPLAY_LAB=1`. The default is 0, and
then no lab declaration or definition exists in `PanelDriver`, the drivers or
`FreeInkDisplay`. Enabling it adds methods only; no existing method, register
sequence, config or LUT table changes.

## API

```cpp
#if FREEINK_CAP_DISPLAY_LAB
if (display.labSupported()) {
  display.labUploadWaveform(tables, sizeof(tables));  // controller layout below
  display.labSetFrameRate(code);                      // UltraChip PLL; false on SSD1677
  uint32_t busyUs = 0;
  display.labScan(plane1, plane2, busyUs);            // one activation
  // ... more scans ...
  display.labEnd();                                   // before any other display call
}
#endif
```

| Call | Effect |
|---|---|
| `labSupported()` | True for `Ssd1677Driver`, `Uc8179Driver` and `Uc8279X4Driver`; false for every other driver (the `PanelDriver` defaults). |
| `labUploadWaveform(bytes, len)` | Loads the waveform into volatile registers. False, with no bus traffic, for a wrong length or table prefix. |
| `labSetFrameRate(code)` | UltraChip: writes `code` verbatim to PLL (0x30). SSD1677: false, since its frame rate is part of the LUT bytes. |
| `labScan(plane1, plane2, busyUs)` | Writes both planes raw and runs one activation; `busyUs` is the time from activation to BUSY release. False, with no bus traffic, until a waveform has been uploaded in this session. |
| `labEnd()` | Restores the registers the lab changed and makes the next ordinary refresh a full resync. A no-op outside a session. |

The facade waits out a pending async refresh before each call. Every call but
`labSupported()` and `labEnd()` returns false while output is inverted, because
the planes are sent as given.

Call `labEnd()` before any other display call. Ordinary refreshes neither
restore the lab's registers (an UltraChip OTP refresh would run at the lab's
PLL rate) nor keep its waveform loaded, so a session that needs a clean
mid-way ends, refreshes and starts again with a new upload.

## Planes

Planes are full-screen 1-bpp buffers in the framebuffer's byte order. They
are not inverted and not combined with any base frame; each driver applies only
the row mapping its ordinary refresh applies to the framebuffer, so a plane
lands where a B/W refresh would put the same bytes.

| Controller | plane1 | plane2 | Mapping |
|---|---|---|---|
| SSD1677 | BW RAM (0x24) | RED RAM (0x26) | The ordinary RAM window: X increments, Y decrements (the panel's reversed gates); `mirrorX`/`mirrorY` mounts apply as usual. |
| UC8179 | DTM1 (0x10) | DTM2 (0x13) | Rows reversed; gates 480-599 padded 0xFF in both planes. |
| UC8279 (X4 Pro) | DTM1 (0x10) | DTM2 (0x13) | Rows forward from gate 120; gates 0-119 padded 0xFF in both planes. |

Which LUT row or table a pixel selects from its two plane bits is the
controller's rule, unchanged by the lab.

## Waveform bytes and register sequences

The lab writes volatile registers only and never sends an OTP or MTP
programming command.

### SSD1677

The waveform is the 112-byte layout of `src/lut/Ssd1677Luts.h`. The upload is
the overlay grayscale path's `setCustomLut()`: bytes 0-104 to 0x32, byte 105
(VGH) to 0x03, bytes 106-108 (VSH1, VSH2, VSL) to 0x04, byte 109 (VCOM) to
0x2C, then the config's gray border value to 0x3C when it has one. Bytes 110-111
are reserved. The voltage bytes are written as supplied, as the existing
`displayGray(lut)` path writes them, so the caller supplies the panel's stock
values.

A scan sets the RAM window (0x11, 0x44, 0x45, 0x4E, 0x4F), writes 0x24 and
0x26, then 0x21=0x00, 0x22=0xCC, 0x20, and waits for BUSY. `labEnd()` sends
nothing: the next refresh is promoted to the clean HALF or FULL, whose OTP
sequence reloads the LUT and voltages and rewrites both planes.

### UltraChip (UC8179, UC8279)

The waveform is five register-prefixed tables of 43 bytes, in this order:
0x20 VCOM, 0x21 WW, 0x22 KW, 0x23 WK, 0x24 KK. Each prefix is followed by 42
data bytes in the controller's LUT format.

| Step | UC8179 | UC8279 (X4 Pro) |
|---|---|---|
| Upload | PSR `psr0` (REG=1), the five tables (42 bytes each), CDI `cdiActive`, 0x07 | PSR `psr0` (REG=1), the five tables padded with 7 zero bytes to the 49-byte LUT register (so no group of an earlier AA bank survives), CDI `cdiAa` |
| Frame rate | PLL `code` | PLL `code`; refused on X4 Classic, whose stock firmware never programs PLL |
| Scan | DTM1, DTM2, PON if off, DRF, BUSY wait | DTM1, DTM2, PON if off, PSR `psr0` (only a post-PON PSR write latches), DRF, BUSY wait |
| `labEnd()` | PSR `psr0 & 0xDF` (OTP), CDI `cdiIdle`, 0x07, PLL 0x06, TSSET `tsset` | PSR `psr0 & 0xDF` (OTP), CDI `cdiBwFull`, PLL `pll` (not on X4 Classic), TSSET `tsset` |

The CDI value is the one the driver's grayscale activation uses, so plane bits
select tables as they do there. Power stays on between scans (no POF). The lab
never writes the power (0x01), booster or VCOM DC (0x82) registers. On UC8179 a
session that starts while direct grayscale owns those registers first resets
the controller back to the stock B/W setup. UC8179 PLL 0x06 is the 50 Hz rate
the driver's grayscale path restores after its slower image pass.

After `labEnd()` both drivers treat the panel as unknown: the next ordinary
refresh is a full GC from a white old plane.

## Tests

`test/host/run_pro.py` (see [Testing](testing.md)) builds the Pro suite with
the lab off and on. With it on, `test_pro.cpp` asserts each driver's exact
upload, frame-rate, scan and `labEnd()` sequences, the raw plane bytes, the
refusals, and the full resync that follows. In both builds a `trace` run hashes
the stock register sequences of all three drivers and the facade, and the script
fails unless the two builds produce identical hashes.
