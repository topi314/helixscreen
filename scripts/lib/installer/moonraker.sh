#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: moonraker
# Moonraker update_manager configuration
#
# Reads: PLATFORM, INSTALL_DIR, SUDO
# Writes: -

# Source guard
[ -n "${_HELIX_MOONRAKER_SOURCED:-}" ] && return 0
_HELIX_MOONRAKER_SOURCED=1

# Common moonraker.conf locations
# ZMOD-on-AD5X notes:
#   /opt/config is a symlink to /usr/data/config; printer_data lives under both.
#   We list both forms so symlink-aware and -unaware path resolutions both hit.
MOONRAKER_CONF_PATHS="
/home/pi/printer_data/config/moonraker.conf
/home/biqu/printer_data/config/moonraker.conf
/home/mks/printer_data/config/moonraker.conf
/root/printer_data/config/moonraker.conf
/opt/config/printer_data/config/moonraker.conf
/opt/config/moonraker.conf
/usr/data/config/printer_data/config/moonraker.conf
/usr/data/printer_data/config/moonraker.conf
"

# Find moonraker.conf
# Returns: path to moonraker.conf or empty string
find_moonraker_conf() {
    # Dynamic: check detected user's home first
    if [ -n "${KLIPPER_HOME:-}" ]; then
        local user_conf="${KLIPPER_HOME}/printer_data/config/moonraker.conf"
        if [ -f "$user_conf" ]; then
            echo "$user_conf"
            return 0
        fi
    fi

    # Static fallback
    for conf in $MOONRAKER_CONF_PATHS; do
        if [ -f "$conf" ]; then
            echo "$conf"
            return 0
        fi
    done
    echo ""
}

# Check if update_manager section for helixscreen already exists
# Args: $1 = moonraker.conf path
# Returns: 0 if exists, 1 if not
has_update_manager_section() {
    local conf="$1"
    grep -q '^\[update_manager helixscreen\]' "$conf" 2>/dev/null
}

# Generate update_manager configuration block
generate_update_manager_config() {
    cat << EOF

# HelixScreen Update Manager
# Added by HelixScreen installer - enables one-click updates from Mainsail/Fluidd
# NOTE: type: web is used instead of type: zip as a workaround for
# mainsail-crew/mainsail#2444 (zip type always shows UP-TO-DATE).
# A systemd path unit handles service restart after Moonraker extracts the update.
[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: ${INSTALL_DIR}
EOF
}

# Add update_manager section to moonraker.conf
# Args: $1 = moonraker.conf path
add_update_manager_section() {
    local conf="$1"
    local fs
    fs=$(file_sudo "$conf")

    # Create backup
    $fs cp "$conf" "${conf}.bak.helixscreen" 2>/dev/null || true

    # Append configuration
    generate_update_manager_config | $fs tee -a "$conf" >/dev/null

    log_success "Added update_manager section to $conf"
    log_info "You can now update HelixScreen from the Mainsail/Fluidd web interface!"
}

# Check if moonraker.conf has old git_repo-style helixscreen section
# Args: $1 = moonraker.conf path
# Returns: 0 if old git_repo section found, 1 if not
has_old_git_repo_section() {
    local conf="$1"
    # Look for helixscreen section with type: git_repo
    if grep -q '^\[update_manager helixscreen\]' "$conf" 2>/dev/null; then
        # Extract the section and check for git_repo type
        awk '/^\[update_manager helixscreen\]/{found=1; next} found && /^\[/{exit} found && /^type:/{print; exit}' "$conf" | grep -q 'git_repo'
        return $?
    fi
    return 1
}

# Check if moonraker.conf has old zip-style helixscreen section
# Args: $1 = moonraker.conf path
# Returns: 0 if old zip section found, 1 if not
has_old_zip_section() {
    local conf="$1"
    if grep -q '^\[update_manager helixscreen\]' "$conf" 2>/dev/null; then
        awk '/^\[update_manager helixscreen\]/{found=1; next} found && /^\[/{exit} found && /^type:/{print; exit}' "$conf" | grep -q 'zip'
        return $?
    fi
    return 1
}

# Migrate old section (git_repo or zip) to type: web
# Args: $1 = moonraker.conf path
migrate_to_web_type() {
    local conf="$1"

    log_info "Migrating update_manager to type: web..."

    # Remove old section
    remove_update_manager_section "$conf" 2>/dev/null || true

    # Add new web section
    add_update_manager_section "$conf"

    # Clean up old sparse clone directory if it exists (from git_repo era)
    local old_repo_dir="${INSTALL_DIR}-repo"
    if [ -d "$old_repo_dir" ]; then
        log_info "Removing old updater repo at $old_repo_dir..."
        $SUDO rm -rf "$old_repo_dir"
    fi

    log_success "Migrated to type: web update manager"
}

# Remove unsupported options from the helixscreen update_manager section.
# type: web only supports: type, channel, repo, path.
# Options like persistent_files, managed_services, and install_script are
# not supported and cause Moonraker to log "unparsed config option" warnings.
# Args: $1 = moonraker.conf path
cleanup_unsupported_options() {
    local conf="$1"

    # Check if any unsupported options exist in the helixscreen section.
    # persistent_files has indented continuation lines; the others are single-line.
    if ! awk '
        /^\[update_manager helixscreen\]/{found=1; next}
        found && /^\[/{exit}
        found && /^(persistent_files|managed_services|install_script):/{print; exit}
    ' "$conf" | grep -q .; then
        return 0
    fi

    log_info "Removing unsupported options from [update_manager helixscreen]"
    local fs
    fs=$(file_sudo "$conf")
    $fs cp "$conf" "${conf}.bak.helixscreen" 2>/dev/null || true

    # Remove matching key: lines within the helixscreen section.
    # persistent_files also has indented continuation lines (4-space indent).
    $fs awk '
        /^\[update_manager helixscreen\]/ { in_section=1 }
        in_section && /^\[/ && !/^\[update_manager helixscreen\]/ { in_section=0 }
        in_section && /^(persistent_files|managed_services|install_script):/ { skip_block=1; next }
        skip_block && /^    / { next }
        skip_block { skip_block=0 }
        { print }
    ' "$conf" > "${conf}.tmp" && $fs mv "${conf}.tmp" "$conf"

    log_success "Cleaned up unsupported options from moonraker.conf"
}

# Write release_info.json if not already present
# Moonraker type:web needs this file to detect installed version
write_release_info() {
    local release_info="${INSTALL_DIR}/release_info.json"

    if [ -f "$release_info" ]; then
        return 0
    fi

    # Try to detect version from binary
    local version=""
    if [ -x "${INSTALL_DIR}/bin/helix-screen" ]; then
        version=$("${INSTALL_DIR}/bin/helix-screen" --version 2>/dev/null | head -n 1 | grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+[^ ]*' || echo "")
    fi

    if [ -z "$version" ]; then
        log_warn "Could not detect version for release_info.json"
        return 0
    fi

    # Resolve the platform-specific asset name through the single source of
    # truth in platform.sh (shared with mk/cross.mk's baked release_info.json,
    # so the two never drift). A wrong/missing asset_name makes Moonraker fall
    # back to the alphabetically-first release asset — a .sym debug file — and
    # die with "File is not a zip file" (prestonbrown/helixscreen#993).
    local asset_name
    asset_name="$(helix_self_update_asset "${PLATFORM:-pi}")"

    log_info "Writing release_info.json (${version})..."
    cat > "${release_info}.tmp" << EOF
{"project_name":"helixscreen","project_owner":"prestonbrown","version":"${version}","asset_name":"${asset_name}"}
EOF
    # Try without sudo first (self-update: INSTALL_DIR is user-owned under NoNewPrivileges).
    # Fall back to sudo for fresh installs where the directory may be root-owned.
    mv "${release_info}.tmp" "$release_info" 2>/dev/null || \
        $SUDO mv "${release_info}.tmp" "$release_info" 2>/dev/null || true
}

# Ensure helixscreen is in moonraker.asvc (service allowlist)
# Moonraker requires services to be listed here before it can manage them.
# The asvc file lives in printer_data/, one level up from config/moonraker.conf.
# Args: $1 = moonraker.conf path (used to derive printer_data path)
ensure_moonraker_asvc() {
    local conf="$1"
    # printer_data is two levels up from config/moonraker.conf
    local printer_data
    printer_data="$(dirname "$(dirname "$conf")")"
    local asvc="${printer_data}/moonraker.asvc"

    if [ ! -f "$asvc" ]; then
        log_info "No moonraker.asvc found at $asvc, skipping"
        return 0
    fi

    if grep -q '^helixscreen$' "$asvc" 2>/dev/null; then
        return 0
    fi

    local fs
    fs=$(file_sudo "$asvc")
    log_info "Adding helixscreen to $asvc..."
    # Ensure file ends with a newline before appending (#408)
    if [ -s "$asvc" ] && [ "$(tail -c 1 "$asvc" | wc -l)" -eq 0 ]; then
        echo "" | $fs tee -a "$asvc" >/dev/null
    fi
    echo "helixscreen" | $fs tee -a "$asvc" >/dev/null
    log_success "Added helixscreen to Moonraker service allowlist"
}

# Restart Moonraker to pick up configuration changes
restart_moonraker() {
    if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet moonraker 2>/dev/null; then
        log_info "Restarting Moonraker to apply configuration..."
        $SUDO systemctl restart moonraker || true
    elif [ -x "/etc/init.d/S56moonraker_service" ]; then
        # K1/Simple AF uses SysV init
        log_info "Restarting Moonraker to apply configuration..."
        if ! $SUDO /etc/init.d/S56moonraker_service restart 2>/dev/null; then
            log_warn "Could not restart Moonraker - you may need to restart it manually"
        fi
    fi
}

# Configure Moonraker update_manager
# Called during installation on platforms with web UI (Pi, K1 with Simple AF)
configure_moonraker_updates() {
    local platform=$1

    # Skip on AD5M (stock Flashforge firmware lacks Mainsail/Fluidd).
    # NOTE: AD5X is intentionally NOT skipped — it's only ever installed via the
    # ZMOD chroot, which ships Mainsail/Fluidd.
    if [ "$platform" = "ad5m" ]; then
        log_info "Skipping Moonraker update_manager on AD5M (typically no web UI)"
        return 0
    fi

    log_info "Configuring Moonraker update_manager..."

    # Write release_info.json if not already present (fallback for older tarballs)
    write_release_info

    local conf
    conf=$(find_moonraker_conf)

    if [ -z "$conf" ]; then
        log_warn "Could not find moonraker.conf in any known location:"
        if [ -n "${KLIPPER_HOME:-}" ]; then
            log_warn "  ${KLIPPER_HOME}/printer_data/config/moonraker.conf"
        fi
        for tried in $MOONRAKER_CONF_PATHS; do
            log_warn "  $tried"
        done
        log_warn "To enable web UI updates, manually add to your moonraker.conf:"
        echo ""
        generate_update_manager_config
        echo ""
        return 0
    fi

    log_info "Using moonraker.conf at: $conf"

    # Migrate old git_repo or zip config to type: web
    # (type: zip shows perpetual UP-TO-DATE in Mainsail — see mainsail-crew/mainsail#2444)
    if has_old_git_repo_section "$conf" || has_old_zip_section "$conf"; then
        migrate_to_web_type "$conf"
        ensure_moonraker_asvc "$conf"
        restart_moonraker
        return 0
    fi

    if has_update_manager_section "$conf"; then
        log_info "update_manager section already exists in $conf"
        # Remove options not supported by type: web (persistent_files,
        # managed_services, install_script) that cause Moonraker warnings.
        cleanup_unsupported_options "$conf"
        # Still ensure asvc is correct even if section already exists
        ensure_moonraker_asvc "$conf"
        return 0
    fi

    add_update_manager_section "$conf"
    ensure_moonraker_asvc "$conf"
    restart_moonraker
}

# Remove update_manager section from moonraker.conf
# Called during uninstallation
remove_update_manager_section() {
    local conf
    conf=$(find_moonraker_conf)

    if [ -z "$conf" ]; then
        return 0
    fi

    if ! has_update_manager_section "$conf"; then
        return 0
    fi

    log_info "Removing update_manager section from $conf..."

    # Create backup
    local fs
    fs=$(file_sudo "$conf")
    $fs cp "$conf" "${conf}.bak.helixscreen-uninstall" 2>/dev/null || true

    # Remove the section (from [update_manager helixscreen] to next section or EOF)
    # This uses awk to skip lines between [update_manager helixscreen] and the next [section]
    $fs sh -c "awk '
        /^\[update_manager helixscreen\]/ { skip=1; next }
        /^\[/ { skip=0 }
        !skip { print }
    ' \"$conf\" > \"${conf}.tmp\"" && $fs mv "${conf}.tmp" "$conf"

    # Also remove any "Added by HelixScreen" comment lines that precede it
    $fs sed -i '/# HelixScreen Update Manager/d' "$conf" 2>/dev/null || \
    $fs sed -i '' '/# HelixScreen Update Manager/d' "$conf" 2>/dev/null || true

    $fs sed -i '/# Added by HelixScreen installer/d' "$conf" 2>/dev/null || \
    $fs sed -i '' '/# Added by HelixScreen installer/d' "$conf" 2>/dev/null || true

    log_success "Removed update_manager section from $conf"
}
