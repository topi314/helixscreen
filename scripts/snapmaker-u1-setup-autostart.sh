#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Set up HelixScreen auto-start on Snapmaker U1
# Run on the device: ./snapmaker-u1-setup-autostart.sh /userdata/helixscreen
#
# This script:
# 1. Creates /oem/.debug to prevent overlay wipe on boot
# 2. Patches /etc/init.d/S99screen to start HelixScreen instead of stock GUI
# 3. Disables the stock UI binary /usr/bin/gui (chmod a-x) so no launcher can
#    exec it
#
# Firmware 1.3 vs 1.4 behavior:
#   - 1.3 ships /etc/init.d/S99screen, the ONLY launcher of /usr/bin/gui. Our
#     S99screen patch (step 2) suppresses the stock UI there.
#   - 1.4 DELETED /etc/init.d/S99screen and launches /usr/bin/gui from a
#     runtime-generated path that is NOT present in the flashed rootfs, so it
#     cannot be patched by name. Patching S99screen alone does not suppress the
#     stock UI on 1.4 — on reboot the stock screen reclaims the display.
#   Step 3 is the launcher-independent fix: disabling the binary itself means
#   no stock launcher on EITHER firmware (1.3's S99screen or 1.4's relocated
#   launcher) can exec /usr/bin/gui, so the stock UI never grabs the
#   framebuffer / DRM. We still write S99screen (step 2) because it is also our
#   HelixScreen LAUNCHER and runs at boot on both firmwares.
#
# The patch is regenerated and compared to the on-disk version every run; if
# they differ we rewrite. This is self-healing: a legacy patch from an older
# helixscreen version (e.g. one that hardcoded the init path before
# helixscreen.init moved into config/) gets repaired on the next self-update,
# and the previous "skip if first 5 lines mention HelixScreen" heuristic that
# silently locked users to a broken patch is gone.
#
# To revert: run the HelixScreen uninstaller — it restores the /usr/bin/gui
# exec bit and the stock S99screen launcher. NOTE: a bare
# `rm -rf /userdata/helixscreen && reboot` is no longer sufficient — it leaves
# /usr/bin/gui disabled, so the stock UI will NOT come back. Use the
# uninstaller to fully revert.

set -e

# Testability sysroot prefix. Empty in production; the BATS suite points it at
# a mock rootfs so the host-system paths this script touches (/oem/.debug,
# /etc/init.d/S99screen, /usr/bin/gui) land under a temp dir instead of the
# real system. Only HOST paths get the prefix — the heredoc PATCH body below
# runs on the device and stays absolute.
SYSROOT="${HELIX_SETUP_ROOT:-}"

DEPLOY_DIR="${1:-/userdata/helixscreen}"

INIT_SCRIPT="$DEPLOY_DIR/config/helixscreen.init"
if [ ! -f "$INIT_SCRIPT" ]; then
    echo "Error: $INIT_SCRIPT not found"
    echo "Deploy HelixScreen first, then run this script"
    exit 1
fi

# Step 1: Create /oem/.debug to prevent overlay wipe on boot
# Without this, S01aoverlayfs runs: rm -rf /oem/overlay/*
if [ ! -f "${SYSROOT}/oem/.debug" ]; then
    touch "${SYSROOT}/oem/.debug"
    echo "Created ${SYSROOT}/oem/.debug (overlay persistence enabled)"
else
    echo "${SYSROOT}/oem/.debug already exists"
fi

# Step 2: Render desired S99screen patch into a temp file
S99_TARGET="${SYSROOT}/etc/init.d/S99screen"
TMP_PATCH=$(mktemp)
trap 'rm -f "$TMP_PATCH"' EXIT

cat > "$TMP_PATCH" << 'PATCH'
#!/bin/sh
#
# Start/stop GUI process
# Modified by HelixScreen: delegates to HelixScreen init when installed
#

GUI="/usr/bin/gui"
PIDFILE=/var/run/gui.pid

log()
{
	logger -p user.info -t "GUI[$$]" -- "$1"
	echo "$1"
}

# If HelixScreen is installed, delegate to its init script
for helix_init in /userdata/helixscreen/config/helixscreen.init /opt/helixscreen/config/helixscreen.init; do
    if [ -x "$helix_init" ]; then
        case "$1" in
          start)
            log "HelixScreen detected, starting instead of stock GUI"
            "$helix_init" start
            ;;
          stop)
            "$helix_init" stop
            ;;
          restart)
            "$helix_init" stop
            sleep 1
            "$helix_init" start
            ;;
          *)
            echo "Usage: $0 {start|stop|restart}"
            exit 1
        esac
        exit 0
    fi
done

# Stock GUI fallback (no HelixScreen installed)
case "$1" in
  start)
	log "Starting GUI process..."
	ulimit -c 102400
	start-stop-daemon -S -b -x "$GUI" -m -p "$PIDFILE"
	;;
  stop)
	log "Stopping GUI process..."
	start-stop-daemon -K -x "$GUI" -p "$PIDFILE" -o
	;;
  restart)
	"$0" stop
	sleep 1
	"$0" start
	;;
  *)
	echo "Usage: $0 {start|stop|restart}"
	exit 1
esac
PATCH

# Step 3: If the on-disk script already matches, skip the rewrite — but DO NOT
# exit, because the gui-disable step (Step 5) must still run on every invocation
# (e.g. a self-update that already has the current S99screen still needs to
# ensure /usr/bin/gui stays disabled on 1.4).
if [ -f "$S99_TARGET" ] && cmp -s "$TMP_PATCH" "$S99_TARGET"; then
    echo "S99screen already patched (current version)"
else
    # Step 4: Preserve the original stock S99screen the first time we replace it.
    # Detect "stock" by absence of any HelixScreen marker; once we've saved a
    # .stock copy we don't overwrite it.
    if [ -f "$S99_TARGET" ] && [ ! -f "$S99_TARGET.stock" ] && \
       ! grep -q HelixScreen "$S99_TARGET" 2>/dev/null; then
        cp "$S99_TARGET" "$S99_TARGET.stock"
        echo "Saved stock S99screen backup to $S99_TARGET.stock"
    fi

    cp "$TMP_PATCH" "$S99_TARGET"
    chmod +x "$S99_TARGET"
    echo "S99screen patched — HelixScreen will auto-start on boot"
fi

# Step 5: Neutralize the stock on-device UI so HelixScreen owns the display.
# 1.3 launches /usr/bin/gui via /etc/init.d/S99screen (patched above); 1.4
# removed S99screen and launches /usr/bin/gui from a runtime path we cannot
# patch by name. Disabling the binary itself is launcher-independent: no stock
# script on either firmware can exec it, so the stock UI never grabs the
# framebuffer / DRM. Reversible — the uninstaller restores the exec bit.
GUI_BIN="${SYSROOT}/usr/bin/gui"
if [ -x "$GUI_BIN" ]; then
    if chmod a-x "$GUI_BIN" 2>/dev/null; then
        echo "Disabled stock UI binary ($GUI_BIN) — HelixScreen owns the display"
    else
        echo "Warning: could not disable $GUI_BIN; stock UI may compete for the display"
    fi
elif [ -f "$GUI_BIN" ]; then
    echo "Stock UI binary already disabled ($GUI_BIN)"
else
    echo "Stock UI binary not present ($GUI_BIN) — nothing to disable"
fi

echo "To revert: run the HelixScreen uninstaller (restores /usr/bin/gui and S99screen)"
