#!/usr/bin/env python3
"""Compile the complete Pro drivers and display facade against a recording bus."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
LIB = HERE.parents[1]
with tempfile.TemporaryDirectory(prefix="freeink-pro-test-") as directory:
    root = Path(directory)
    for folder in ("src/driver", "src/bus", "src/lut", "include"):
        (root / folder).mkdir(parents=True, exist_ok=True)
    drivers = ("Ssd1677", "Uc8179", "Uc8279X4")
    for name in drivers:
        for ext in ("h", "cpp"):
            shutil.copy2(LIB / f"src/driver/{name}Driver.{ext}", root / f"src/driver/{name}Driver.{ext}")
    shutil.copy2(LIB / "src/driver/PanelDriver.h", root / "src/driver/PanelDriver.h")
    shutil.copy2(LIB / "src/FreeInkDisplay.cpp", root / "src/FreeInkDisplay.cpp")
    shutil.copy2(LIB / "include/FreeInkDisplay.h", root / "include/FreeInkDisplay.h")
    shutil.copy2(LIB / "include/GrayscaleCapabilities.h", root / "include/GrayscaleCapabilities.h")
    for name in ("Ssd1677Luts.h", "Uc8279X3Luts.h", "UltraChipDirectGrayLuts.h"):
        shutil.copy2(LIB / f"src/lut/{name}", root / f"src/lut/{name}")
    for name in ("Arduino.h", "BoardConfig.h", "SPI.h", "esp_heap_caps.h", "sdkconfig.h"):
        shutil.copy2(HERE / "pro_stubs" / name, root / name)
    shutil.copy2(HERE / "pro_stubs/EpdBus.h", root / "src/bus/EpdBus.h")
    # Every configuration builds with the display lab compiled out and in. The
    # "trace" run hashes stock register sequences; both builds must agree.
    for single in (False, True):
        traces = {}
        for lab in (0, 1):
            exe = root / (("single" if single else "dual") + f"-lab{lab}")
            command = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                       "-Wno-unused-parameter", "-Wno-unused-function", "-DBOARD_HAS_PSRAM=1",
                       "-DARDUINO=1", "-DFREEINK_DEVICE_METALIO_EINK4=1", f"-DFREEINK_CAP_DISPLAY_LAB={lab}",
                       "-I"+str(root), "-I"+str(root / "include")]
            if single:
                command += ["-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1"]
            command += [str(HERE / "test_pro.cpp"), str(root / "src/FreeInkDisplay.cpp")]
            command += [str(root / f"src/driver/{name}Driver.cpp") for name in drivers]
            subprocess.run(command + ["-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            subprocess.run([str(exe), "sticky"], check=True)
            subprocess.run([str(exe), "metalio"], check=True)
            traces[lab] = subprocess.run([str(exe), "trace"], check=True, capture_output=True, text=True).stdout
        if not traces[0] or traces[0] != traces[1]:
            raise SystemExit("stock register sequences differ with FREEINK_CAP_DISPLAY_LAB=1")
        print(f"Stock sequences byte-identical with the display lab off and on ({traces[0].count(chr(10))} steps)")
