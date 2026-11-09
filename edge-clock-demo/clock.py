#!/usr/bin/env python3
"""A one-dimensional, single-row clock; no third-party dependencies."""

import argparse
from datetime import datetime
import os
import sys
import time


WIDTH = 61	# one cell per dial minute, so a tick index is a cell index
MINUTE_WIDTH = 3
HOUR_WIDTH = 5
RESET = "\033[0m"
DIM = "\033[2m"
HOUR = "\033[2;36m"
MINUTE = "\033[1m"


def parse_time(value):
	try:
		if len(value) != 5 or value[2] != ":":
			raise ValueError
		return datetime.strptime(value, "%H:%M")
	except ValueError:
		raise argparse.ArgumentTypeError("expected HH:MM, from 00:00 to 23:59") from None


def render(hour, minute, styled=False):
	if not 0 <= hour < 24 or not 0 <= minute < 60:
		raise ValueError("time out of range")

	cells = [" "] * WIDTH
	styles = [""] * WIDTH

	# Hands are segments of the strap, not points, and they wrap around the ends
	# so a hand near 0 or 60 still lights up the far corner.
	for index, char, style, span in (
		((hour % 12) * 5, "-", HOUR, HOUR_WIDTH),
		(minute, "=", MINUTE, MINUTE_WIDTH),
	):
		for offset in range(span // 2, span // 2 - span, -1):
			cell = (index + offset) % WIDTH
			cells[cell] = char
			styles[cell] = style

	# Separators win over both hands, so a hand that lands on a mark shows the
	# mark through it ("stuck on the mark") and never hides it.
	for tick in (0, 15, 30, 45, 60):
		cells[tick] = "$"
		styles[tick] = DIM
	assert "=" in cells and all(cells[tick] == "$" for tick in (0, 15, 30, 45, 60))

	if not styled:
		return "".join(cells)
	parts = []
	previous_style = None
	for char, style in zip(cells, styles):
		if style != previous_style:
			parts.append(RESET + style)
			previous_style = style
		parts.append(char)
	return "".join(parts) + RESET


def run():
	previous = None
	try:
		sys.stdout.write("\033[?25l")
		while True:
			now = datetime.now()
			line = render(now.hour, now.minute, styled=True)
			if line != previous:
				sys.stdout.write("\r\033[2K" + line)
				sys.stdout.flush()
				previous = line
			time.sleep(1)
	except KeyboardInterrupt:
		pass
	finally:
		sys.stdout.write(RESET + "\033[?25h\n")
		sys.stdout.flush()


def main():
	parser = argparse.ArgumentParser(description="Single-row clock; Ctrl+C to exit.")
	parser.add_argument("--at", type=parse_time, metavar="HH:MM", help="print a fixed local time and exit")
	args = parser.parse_args()
	ansi = sys.stdout.isatty() and os.environ.get("TERM") != "dumb"
	if args.at is not None:
		print(render(args.at.hour, args.at.minute, styled=ansi))
	else:
		if not ansi:
			parser.error("live mode needs an ANSI terminal; use --at HH:MM for plain output")
		run()


if __name__ == "__main__":
	main()
