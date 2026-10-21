#!/usr/bin/env python3
"""Run USB idle policy regression checks with a native C compiler, no Pico needed."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

FIRMWARE = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="picoarc-idle-") as directory:
        binary = Path(directory) / "idle_audio_test"
        common = [
            *shlex.split(os.environ.get("CC", "cc")), "-std=c11",
            "-DCFG_TUSB_MCU=OPT_MCU_NONE", "-DCFG_TUSB_OS=OPT_OS_NONE",
            "-DTUP_DCD_ENDPOINT_MAX=8", "-DPICOARC_DEBUG_USB=0",
            "-DPICOARC_LOGGING=0", "-DPICOARC_USB_DIAGNOSTICS=0",
            "-U__ARM_FEATURE_UNALIGNED",
            f"-I{FIRMWARE / 'src'}", f"-I{FIRMWARE / 'third_party/tinyusb/src'}",
            f"-I{FIRMWARE / 'tests/stubs'}",
            str(FIRMWARE / "src/usb_audio.c"),
            str(FIRMWARE / "src/usb_descriptors.c"),
            str(FIRMWARE / "src/audio_rates.c"),
            str(FIRMWARE / "src/audio_samples.c"),
            str(FIRMWARE / "tests/idle_audio_test.c"), "-o", str(binary),
        ]
        for uac in (1, 2):
            for timeout in (None, 0, 1, -1, 86400):
                flags = [f"-DPICOARC_UAC_VERSION={uac}"]
                if timeout is not None:
                    flags.append(f"-DPICOARC_IDLE_AUDIO_TIMEOUT_SECONDS={timeout}")
                print(f"UAC{uac}, timeout={timeout if timeout is not None else 'default'}", flush=True)
                subprocess.run(common + flags, check=True)
                subprocess.run([str(binary)], check=True)
        invalid = subprocess.run(common + ["-DPICOARC_UAC_VERSION=1",
            "-DPICOARC_IDLE_AUDIO_TIMEOUT_SECONDS=-2"], capture_output=True, text=True)
        assert invalid.returncode != 0
        assert "must be -1, 0, or positive seconds" in invalid.stderr
        print("Invalid negative timeout rejected")


if __name__ == "__main__":
    main()
