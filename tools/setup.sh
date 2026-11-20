#!/bin/sh
# Set up the drive to play: see tools/setup.py (README, Play the drive).
exec python3 "$(dirname "$0")/setup.py" "$@"
