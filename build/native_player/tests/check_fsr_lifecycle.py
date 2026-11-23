"""Check a cold FSR launch, a field resize away and back, and entry into Summons.

Record with REINCOMP_TRACE_FSR=1 and stay at each size until fsr31.log reports
an output=direct frame. Pass fsr31.log and frame-state.log from the same run.
"""

import argparse
from pathlib import Path
import re


def check(fsr, frames):
    field = re.search(r"\bframe=(\d+) scene=(-?\d+) .*?actor=3D highest=1 loaded=1", frames)
    if not field or "arrived at GachaTopScreenView" not in frames:
        raise ValueError("The capture must reach the loaded 3D field and the Summons screen.")
    scenes = [(int(frame), handle, name) for frame, handle, name in
              re.findall(r"^Frame (\d+) active scene (-?\d+) (\S+)", frames, re.M)]
    field_start = max((frame for frame, handle, _ in scenes
                       if frame <= int(field[1]) and handle == field[2]), default=None)
    menu_start = next((frame for frame, _, name in scenes if frame > int(field[1]) and name == "default"), None)
    if field_start is None or menu_start is None or "Rendering menu scene at native resolution" not in fsr:
        raise ValueError("Missing native menu transition.")
    menu_end = next((frame for frame, _, name in scenes if frame > menu_start and name != "default"), float("inf"))

    size = None
    startup = False
    field_sizes = []
    for line in fsr.splitlines():
        if "FSR disabled" in line or "FSR using copied output:" in line:
            raise ValueError(line)
        ready = re.match(r"AMD FSR 3.1 ready: \d+x\d+ -> (\d+x\d+)$", line)
        if ready:
            size = ready[1]
        sample = re.match(r"Frame (\d+) camera=.* output=direct$", line)
        if not sample:
            continue
        frame = int(sample[1])
        if frame < field_start:
            startup = True
        elif frame < menu_start and size and (not field_sizes or field_sizes[-1] != size):
            field_sizes.append(size)
        elif menu_start <= frame < menu_end:
            raise ValueError(f"FSR reconstructed frame {frame} in the native menu.")
    if not startup:
        raise ValueError("No direct-output frame during startup.")
    if not field_sizes or field_sizes[0] not in field_sizes[2:]:
        raise ValueError(f"No verified field resize away and back. Observed sizes: {field_sizes}")
    return field_sizes


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fsr_log", type=Path)
    parser.add_argument("frame_log", type=Path)
    args = parser.parse_args()
    try:
        sizes = check(args.fsr_log.read_text(errors="replace"), args.frame_log.read_text(errors="replace"))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print("Direct output at startup and through", " -> ".join(sizes), "in the field. Summons rendered natively.")
