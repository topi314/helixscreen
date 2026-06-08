#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for snapmaker-u1-setup-autostart.sh (display takeover) across PAXX
# Extended Firmware 1.3 and 1.4, plus the matching uninstall restore block.
#
# Firmware difference under test:
#   1.3 ships /etc/init.d/S99screen (the only launcher of /usr/bin/gui).
#   1.4 deleted S99screen and launches /usr/bin/gui from a relocated path.
# The launcher-independent fix disables /usr/bin/gui (chmod a-x) so neither
# firmware can start the stock UI; the script still writes S99screen as the
# HelixScreen launcher. The autostart script is driven against a mock rootfs
# via HELIX_SETUP_ROOT.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    log_info() { echo "INFO: $*"; }
    log_warn() { echo "WARN: $*"; }
    log_success() { echo "OK: $*"; }
    export -f log_info log_warn log_success

    export MOCK_ROOT="$BATS_TEST_TMPDIR/u1"
    mkdir -p "$MOCK_ROOT/oem" "$MOCK_ROOT/etc/init.d" "$MOCK_ROOT/usr/bin"

    # The deploy dir must hold an executable config/helixscreen.init or the
    # autostart script bails out early.
    export DEPLOY_DIR="$BATS_TEST_TMPDIR/deploy"
    mkdir -p "$DEPLOY_DIR/config"
    cat > "$DEPLOY_DIR/config/helixscreen.init" <<'EOF'
#!/bin/sh
echo "helixscreen init $1"
EOF
    chmod +x "$DEPLOY_DIR/config/helixscreen.init"

    # Stock UI binary present and executable, as it ships on the device.
    cat > "$MOCK_ROOT/usr/bin/gui" <<'EOF'
#!/bin/sh
echo "stock gui"
EOF
    chmod +x "$MOCK_ROOT/usr/bin/gui"

    AUTOSTART="$WORKTREE_ROOT/scripts/snapmaker-u1-setup-autostart.sh"
}

# Pre-create a stock S99screen (no HelixScreen marker, launches /usr/bin/gui),
# matching firmware 1.3.
seed_stock_s99screen() {
    cat > "$MOCK_ROOT/etc/init.d/S99screen" <<'EOF'
#!/bin/sh
# Start/stop GUI process
start-stop-daemon -S -b -x /usr/bin/gui -m -p /var/run/gui.pid
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99screen"
}

run_autostart() {
    HELIX_SETUP_ROOT="$MOCK_ROOT" bash "$AUTOSTART" "$DEPLOY_DIR"
}

@test "autostart 1.4: no pre-existing S99screen — disables gui, creates marked launcher" {
    # Firmware 1.4: /etc/init.d/S99screen is absent in the flashed rootfs.
    [ ! -e "$MOCK_ROOT/etc/init.d/S99screen" ]

    run run_autostart
    [ "$status" -eq 0 ]

    # Overlay persistence enabled.
    [ -f "$MOCK_ROOT/oem/.debug" ]

    # The launcher-independent kill switch fired: gui is no longer executable.
    [ -f "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]

    # HelixScreen launcher was created and carries the marker.
    [ -f "$MOCK_ROOT/etc/init.d/S99screen" ]
    grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99screen"
}

@test "autostart 1.3: pre-existing stock S99screen — backs it up, patches it, disables gui" {
    seed_stock_s99screen

    run run_autostart
    [ "$status" -eq 0 ]

    # Stock launcher preserved.
    [ -f "$MOCK_ROOT/etc/init.d/S99screen.stock" ]
    grep -q "start-stop-daemon" "$MOCK_ROOT/etc/init.d/S99screen.stock"
    ! grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99screen.stock"

    # Live launcher now ours.
    grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99screen"

    # Binary kill switch fired on 1.3 too.
    [ -f "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]
}

@test "autostart idempotent: second run exits 0, gui stays disabled, .debug present" {
    run run_autostart
    [ "$status" -eq 0 ]
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]

    # Second run with the launcher already current must still succeed and must
    # not re-enable gui (the early-skip must not bypass the gui-disable step).
    run run_autostart
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "already patched"
    [ -f "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]
    [ -f "$MOCK_ROOT/oem/.debug" ]
}

@test "autostart: gui already disabled is reported, not re-enabled" {
    chmod a-x "$MOCK_ROOT/usr/bin/gui"
    run run_autostart
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "already disabled"
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]
}

# --- Uninstall restore block -------------------------------------------------
# Extract just the snapmaker-u1 branch from uninstall.sh and exercise it against
# the mock rootfs, using the same path-rewriting trick as test_cc1_uninstall.

extract_u1_restore() {
    cat > "$BATS_TEST_TMPDIR/u1_restore.sh" <<'SH_EOF'
#!/bin/sh
SH_EOF
    awk '
        /^    # Snapmaker U1: re-enable the stock UI/ { capture=1 }
        capture { print }
        capture && /^    fi$/ && ++blocks==1 { exit }
    ' "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh" >> "$BATS_TEST_TMPDIR/u1_restore.sh"

    # Portable in-place rewrite (BSD sed on macOS needs a suffix arg for -i;
    # GNU sed does not — sidestep both by writing through a temp file).
    sed \
        -e "s|/usr/bin/gui|$MOCK_ROOT/usr/bin/gui|g" \
        -e "s|/etc/init.d/S99screen|$MOCK_ROOT/etc/init.d/S99screen|g" \
        "$BATS_TEST_TMPDIR/u1_restore.sh" > "$BATS_TEST_TMPDIR/u1_restore.sh.tmp"
    mv "$BATS_TEST_TMPDIR/u1_restore.sh.tmp" "$BATS_TEST_TMPDIR/u1_restore.sh"

    u1_restore() {
        local platform="snapmaker-u1"
        local restored_ui=""
        # shellcheck disable=SC1091
        . "$BATS_TEST_TMPDIR/u1_restore.sh"
        printf '%s' "$restored_ui"
    }
}

@test "uninstall u1 (1.4): re-enables gui and removes HelixScreen-created S99screen" {
    # Post-install 1.4 state: gui disabled, our marked S99screen, no .stock.
    chmod a-x "$MOCK_ROOT/usr/bin/gui"
    cat > "$MOCK_ROOT/etc/init.d/S99screen" <<'EOF'
#!/bin/sh
# Modified by HelixScreen
exec /userdata/helixscreen/config/helixscreen.init "$@"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99screen"

    extract_u1_restore
    run u1_restore
    [ "$status" -eq 0 ]
    # restored_ui is set (the marker substring survives the mock-path rewrite).
    echo "$output" | grep -q "Snapmaker stock UI"

    # gui re-enabled, our launcher removed (1.4 had none to restore).
    [ -x "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -e "$MOCK_ROOT/etc/init.d/S99screen" ]
}

@test "uninstall u1 (1.3): re-enables gui and restores stock S99screen from .stock" {
    chmod a-x "$MOCK_ROOT/usr/bin/gui"
    cat > "$MOCK_ROOT/etc/init.d/S99screen" <<'EOF'
#!/bin/sh
# Modified by HelixScreen
exec /userdata/helixscreen/config/helixscreen.init "$@"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99screen"
    cat > "$MOCK_ROOT/etc/init.d/S99screen.stock" <<'EOF'
#!/bin/sh
start-stop-daemon -S -b -x /usr/bin/gui -m -p /var/run/gui.pid
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99screen.stock"

    extract_u1_restore
    run u1_restore
    [ "$status" -eq 0 ]

    [ -x "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -e "$MOCK_ROOT/etc/init.d/S99screen.stock" ]
    [ -f "$MOCK_ROOT/etc/init.d/S99screen" ]
    grep -q "start-stop-daemon" "$MOCK_ROOT/etc/init.d/S99screen"
    ! grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99screen"
}
