#!/usr/bin/env bash
#
# install.sh - set up the RockBLOCK 9704 host tools.
#
# Makes python/.venv, installs this package into it with Ground Control's
# rockblock9704 binding and pytest (`pip install -e .[modem,test]`), checks
# the imports, and checks the two things that stop a USB serial port
# opening on Linux: the dialout group, and ModemManager probing the port.
#
# Usage:
#   python/install.sh            the venv and the checks, no root
#   python/install.sh --udev     ... and a udev rule (sudo) that keeps
#                                ModemManager off the 9704 and names it
#                                /dev/rockblock9704
#   python/install.sh --fresh    delete the venv and start again
#
# Then python/.venv/bin/rb9704, python/.venv/bin/rb9704-fake, and
# python/.venv/bin/pytest (docs/TESTING.md).
#
# Knobs:
#   PYTHON     the interpreter for the venv, default python3 (3.10 to 3.14;
#              rockblock9704 ships wheels for those and nothing else)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
VENV="$HERE/.venv"
PY="${PYTHON:-python3}"
UDEV=""
FRESH=""

for arg in "$@"; do
	case "$arg" in
		--udev) UDEV=1 ;;
		--fresh) FRESH=1 ;;
		-h|--help) sed -n '2,/^set -e/p' "$0" | sed 's/^# \{0,1\}//; /^set -e/d'; exit 0 ;;
		*) echo "!! unknown option $arg"; exit 1 ;;
	esac
done

command -v "$PY" >/dev/null 2>&1 || { echo "!! $PY not found"; exit 1; }
"$PY" -c 'import sys; sys.exit(not ((3, 10) <= sys.version_info[:2] <= (3, 14)))' || {
	echo "!! $("$PY" --version) is outside 3.10 to 3.14, where rockblock9704 has wheels."
	echo "   Set PYTHON to another interpreter."
	exit 1
}
"$PY" -c 'import venv, ensurepip' 2>/dev/null || {
	echo "!! $PY has no venv module. On Debian and Ubuntu: sudo apt install python3-venv"
	exit 1
}

if [ -n "$FRESH" ] && [ -d "$VENV" ]; then
	echo "== removing $VENV"
	rm -rf "$VENV"
fi
if [ ! -x "$VENV/bin/python" ]; then
	echo "== making $VENV with $("$PY" --version)"
	"$PY" -m venv "$VENV"
fi
echo "== installing rockblock9704-tools with the binding and pytest"
"$VENV/bin/python" -m pip install --quiet --upgrade pip
"$VENV/bin/python" -m pip install --quiet -e "$HERE[modem,test]"
"$VENV/bin/python" - <<'EOF'
import importlib.metadata as md
import rockblock9704, serial
print(f"   rockblock9704 {md.version('rockblock9704')}, pyserial {serial.__version__}")
EOF

echo "== checks"
if [ "$(uname)" = Linux ]; then
	if id -nG | tr ' ' '\n' | grep -qx dialout; then
		echo "   ok: $(id -un) is in dialout"
	else
		echo "!! $(id -un) is not in dialout, so /dev/ttyUSB* will not open."
		echo "   sudo usermod -aG dialout $(id -un), then log out and in again."
	fi
	if systemctl is-active --quiet ModemManager 2>/dev/null; then
		if [ -f /etc/udev/rules.d/99-rockblock9704.rules ]; then
			echo "   ok: ModemManager runs, and the udev rule keeps it off the 9704"
		elif [ -z "$UDEV" ]; then
			echo "!! ModemManager is running and may open the 9704's port for a"
			echo "   few seconds after it is plugged in. Run this with --udev to stop"
			echo "   it, or wait 30 s after plugging in before the first command."
		fi
	fi
fi

if [ -n "$UDEV" ]; then
	RULE=/etc/udev/rules.d/99-rockblock9704.rules
	echo "== writing $RULE (sudo)"
	sudo tee "$RULE" >/dev/null <<'EOF'
# RockBLOCK 9704: FTDI FT234XD on the USB-C port. Keep ModemManager from
# probing it, and give it a stable name. Written by rockblock9704-zephyr's
# python/install.sh.
SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6015", ENV{ID_MM_DEVICE_IGNORE}="1", SYMLINK+="rockblock9704", MODE="0660", GROUP="dialout"
EOF
	sudo udevadm control --reload-rules
	sudo udevadm trigger --subsystem-match=tty
	echo "   done; unplug and replug the 9704 if it was attached"
	echo "   (this matches every FT234XD, not only the 9704's)"
fi

echo
echo "Run it as $VENV/bin/rb9704, or put $VENV/bin on the PATH."
echo "First command: rb9704 ports"
