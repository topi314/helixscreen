#!/usr/bin/env bash
# Aggregate `DROPPED` events from UpdateQueue across device logs.
#
# Under current semantics, `scoped_freeze()` BUFFERS callbacks rather than
# dropping them — they splice into pending_ when the freeze releases. So
# `DROPPED (frozen)` events are now historical only (from binaries built
# before commit XXXXXX) and should be zero on any current build.
#
# `DROPPED (shutdown)` events are still possible (post-shutdown enqueues are
# unrecoverable) and indicate a different class of bug: a BG thread that
# kept running past shutdown teardown.
#
# This script aggregates both kinds across one or more log sources so we
# can spot regressions without manually walking each device.
#
# Usage:
#   scripts/freeze-drops.sh <log-file> [<log-file> ...]
#   scripts/freeze-drops.sh --ssh <host> [--ssh <host> ...]
#   scripts/freeze-drops.sh --bundle <debug-bundle.json> [...]
#
# Examples:
#   # Aggregate across multiple devices via SSH
#   scripts/freeze-drops.sh --ssh root@192.168.1.74 --ssh root@192.168.30.103
#
#   # Aggregate from saved debug bundles
#   scripts/freeze-drops.sh --bundle /tmp/debug-bundle-*.json
#
#   # Local file
#   scripts/freeze-drops.sh /tmp/helixscreen.log
#
# Cross-references each tag to its source location.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

inputs=()
mode="files"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ssh)   mode="ssh";    inputs+=("$2"); shift 2 ;;
        --bundle) mode="bundle"; inputs+=("$2"); shift 2 ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) inputs+=("$1"); shift ;;
    esac
done

if [[ ${#inputs[@]} -eq 0 ]]; then
    echo "Usage: $0 <log-file> [...]  |  --ssh <host> [...]  |  --bundle <file.json> [...]" >&2
    exit 1
fi

agg="$TMPDIR/all.txt"
: > "$agg"

for src in "${inputs[@]}"; do
    case "$mode" in
        ssh)
            # Helix-screen logs land in one of three places depending on platform:
            #   - /tmp/helixscreen.log (Snapmaker, AD5M, most file-mode platforms)
            #   - /var/log/messages   (Tina Linux SysV — AD5M syslog mirror)
            #   - logread             (OpenWrt/Tina busybox in-memory syslog — K2, CC1)
            # Collect from all three so the script doesn't silently miss a platform.
            ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=no "$src" \
                "grep 'DROPPED (frozen)' /tmp/helixscreen.log 2>/dev/null; \
                 grep 'DROPPED (frozen)' /var/log/messages 2>/dev/null; \
                 logread 2>/dev/null | grep 'DROPPED (frozen)' 2>/dev/null; \
                 true" \
                2>/dev/null | sed "s|^|[$src] |" >> "$agg" || \
                echo "WARN: could not fetch from $src" >&2
            ;;
        bundle)
            # Bundles store logs under crash_txt or system.log
            if command -v jq >/dev/null 2>&1; then
                jq -r '.crash_txt // .system.log // empty' "$src" 2>/dev/null \
                    | grep 'DROPPED (frozen)' \
                    | sed "s|^|[$(basename "$src")] |" >> "$agg" || true
            else
                echo "WARN: jq not installed, skipping bundle $src" >&2
            fi
            ;;
        files)
            grep 'DROPPED (frozen)' "$src" 2>/dev/null \
                | sed "s|^|[$(basename "$src")] |" >> "$agg" || true
            ;;
    esac
done

total="$(wc -l < "$agg" | tr -d ' ')"
if [[ "$total" -eq 0 ]]; then
    echo "✓ No freeze-drop events found across ${#inputs[@]} source(s)."
    exit 0
fi

echo "═══ L081 Mechanism D freeze-drops ═══"
echo "Total events: $total across ${#inputs[@]} source(s)"
echo ""
echo "──── By tag (drops × tag) ────"
# Extract the tag (after "DROPPED (frozen): ") and aggregate
sed -E 's/.*DROPPED \(frozen\): //' "$agg" | sort | uniq -c | sort -rn

echo ""
echo "──── Source location ────"
# For each unique tag, find its definition in the source tree.
sed -E 's/.*DROPPED \([a-z]+\): //' "$agg" | sort -u | while read -r tag; do
    [[ -z "$tag" ]] && continue
    hit="$(cd "$REPO_ROOT" && grep -rn "\"$tag\"" src include 2>/dev/null \
        | grep -E 'defer|queue_' | head -1)"
    if [[ -z "$hit" ]]; then
        echo "  $tag → (no source match — stale tag from older code path)"
        continue
    fi
    file_line="$(echo "$hit" | cut -d: -f1-2)"
    echo "  $tag"
    echo "    $file_line"
done
echo ""
echo "Note: under the buffer-not-drop UpdateQueue, 'DROPPED (frozen)' should"
echo "be zero on current builds — non-zero results are events from older"
echo "binaries still in the log. 'DROPPED (shutdown)' indicates a BG thread"
echo "that kept enqueueing past shutdown teardown (still a real bug)."

echo ""
echo "──── Per-source breakdown ────"
# First bracketed field is the source tag we prepended.
awk -F'[][]' '{print $2}' "$agg" | sort | uniq -c | sort -rn
