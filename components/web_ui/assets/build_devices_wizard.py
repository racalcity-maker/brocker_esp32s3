#!/usr/bin/env python3
import sys
from pathlib import Path


def main():
    if len(sys.argv) < 3:
        print("Usage: build_devices_wizard.py <output> <part1> [part2...]", file=sys.stderr)
        return 1
    out_path = Path(sys.argv[1])
    parts = [Path(p) for p in sys.argv[2:]]
    chunks = []
    for part in parts:
        if not part.exists():
            raise FileNotFoundError(f"Wizard part not found: {part}")
        content = part.read_text()
        chunks.append(content if content.endswith("\n") else content + "\n")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("".join(chunks))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
