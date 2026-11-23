"""Verify reversible actor movement in a recorded 2D or 3D Proton session."""

import argparse
import math
from pathlib import Path
import re


ACTOR = re.compile(r"^frame=(\d+) actor=(\w+) position=([-\d.,]+)$")
MOVEMENT = re.compile(r"^frame=(\d+) movement phase=(\d+) position=(-?\d+),(-?\d+)")
SCENE = re.compile(r"\bframe=(\d+) scene=(-?\d+) .*?\bactor=(\w+) highest=\d+ loaded=(\d+)")


def read_input(path):
    positions = {}
    gestures = []
    with path.open(errors="replace") as trace:
        for line in trace:
            actor = ACTOR.match(line)
            if actor:
                frame, address, xyz = actor.groups()
                positions.setdefault(int(frame), {})[address] = tuple(map(float, xyz.split(",")))
                continue
            movement = MOVEMENT.match(line)
            if movement:
                frame, phase, x, y = map(int, movement.groups())
                if phase == 0:
                    gestures.append([])
                if gestures:
                    gestures[-1].append((frame, phase, x, y))
    return positions, gestures[-2:]


def check(scene, input_log, frame_log):
    positions, gestures = read_input(input_log)
    if len(gestures) != 2:
        raise ValueError("Record two key holds with tracing enabled.")
    for gesture, sign in zip(gestures, (1, -1)):
        start, end = gesture[0], gesture[-1]
        if end[1] != 3 or end[0] <= start[0]:
            raise ValueError("The input trace is missing a complete key hold.")
        axis = 2 if scene == "2D" else 3  # Screen X for D/A, screen Y for W/S.
        if not any(sign * (event[axis] - start[axis]) > 0 for event in gesture if event[1] == 1):
            raise ValueError(f"Input direction does not match the {scene} test.")

    first, second = gestures
    idle = sorted(frame for frame in positions if frame < first[0][0])
    if len(idle) < 2:
        raise ValueError("Record at least two idle actor samples before moving.")
    frames = idle[-2:]
    for gesture in gestures:
        held = [frame for frame in positions if gesture[0][0] < frame < gesture[-1][0]]
        if not held:
            raise ValueError("Hold each key long enough to record actor positions.")
        frames.append(max(held))

    states = [m.groups() for m in SCENE.finditer(frame_log.read_text(errors="replace"))]
    prior = [state for state in states if int(state[0]) <= frames[0]]
    during = [state for state in states if frames[0] < int(state[0]) <= second[-1][0]]
    if not prior or not during:
        raise ValueError("The frame log does not cover the movement test.")
    scene_id = prior[-1][1]
    if any(state[1:] != (scene_id, scene, "1") for state in prior[-1:] + during):
        raise ValueError(f"The movement test must stay in one loaded {scene} scene.")

    baseline, start, forward, reverse = (positions[frame] for frame in frames)
    matches = []
    for actor in sorted(baseline.keys() & start.keys() & forward.keys() & reverse.keys()):
        a, b, c = start[actor], forward[actor], reverse[actor]
        if math.dist(baseline[actor], a) > 0.2 or min(math.dist(a, b), math.dist(b, c)) <= 0.5:
            continue
        if sum((y - x) * (z - y) for x, y, z in zip(a, b, c)) < 0:
            matches.append((actor, a, b, c))
    if not matches:
        raise ValueError("No stationary actor moved and reversed with the two key holds.")
    return matches


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", choices=("2D", "3D"))
    parser.add_argument("game", type=Path, help="the installed .bunker-player/game directory")
    args = parser.parse_args()
    try:
        actors = check(args.scene, args.game / "desktop-input.log", args.game / "frame-state.log")
    except (ValueError, OSError) as error:
        parser.exit(1, f"{error}\n")
    for actor, idle, forward, reverse in actors:
        print(f"PASS {args.scene} actor {actor}: {idle} -> {forward} -> {reverse}")
