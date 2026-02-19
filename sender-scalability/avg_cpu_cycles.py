#!/usr/bin/env python3
import argparse
import re
import sys
from pathlib import Path


PATTERN = re.compile(r"\bavg_cpu_cycles\b\s*([0-9]+)")


def main() -> int:
	ap = argparse.ArgumentParser(
		description="Compute average of avg_cpu_cycles values in a log file."
	)
	ap.add_argument(
		"path",
		nargs="?",
		default="log_FCScale.txt",
		help="Path to log file (default: log_FCScale.txt)",
	)
	args = ap.parse_args()

	path = Path(args.path)
	if not path.exists():
		print(f"error: file not found: {path}", file=sys.stderr)
		return 2

	total = 0
	count = 0
	with path.open("r", encoding="utf-8", errors="replace") as f:
		for line in f:
			m = PATTERN.search(line)
			if not m:
				continue
			total += int(m.group(1))
			count += 1

	if count == 0:
		print("no avg_cpu_cycles values found", file=sys.stderr)
		return 1

	avg = total / count
	print(f"count {count}")
	print(f"avg {avg:.6f}")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())

