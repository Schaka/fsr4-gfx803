#!/bin/bash
# Work out which AMD card to run on and how the picture reaches a monitor.
#
# Source this, then use $GPU_PCI, $GPU_NAME and $DRM_DEVICES.
#
# Nothing here is hardcoded, because this box has had a card swapped under it and the DRM numbering
# moved with it. Two things have to be decided rather than assumed:
#
#   which card runs the work   the one AMD render node, by PCI id, for MESA_VK_DEVICE_SELECT
#   which card scans out       the AMD card itself when a monitor is on it, otherwise the AMD card
#                              plus whichever card does have one, so the compositor can copy across
#
# Getting the second one wrong gives a working compositor drawing a cursor over a black frame.

GPU_CARD=""
for c in /sys/class/drm/card[0-9]*; do
    [ -e "$c/device/vendor" ] || continue
    [ "$(cat "$c/device/vendor")" = 0x1002 ] || continue
    GPU_CARD=$(basename "$c")
    break
done
if [ -z "$GPU_CARD" ]; then
    echo "gpu_env: no AMD card found" >&2
    return 1 2>/dev/null || exit 1
fi

GPU_DEV=$(cat /sys/class/drm/$GPU_CARD/device/device)          # e.g. 0x687f
GPU_PCI="1002:${GPU_DEV#0x}"

# Does the AMD card drive a monitor itself?
GPU_HAS_OUTPUT=no
for s in /sys/class/drm/$GPU_CARD-*/status; do
    [ -e "$s" ] || continue
    [ "$(cat "$s")" = connected ] && { GPU_HAS_OUTPUT=yes; break; }
done

if [ "$GPU_HAS_OUTPUT" = yes ]; then
    DRM_DEVICES=/dev/dri/$GPU_CARD
else
    SCANOUT=""
    for c in /sys/class/drm/card[0-9]*; do
        n=$(basename "$c")
        [ "$n" = "$GPU_CARD" ] && continue
        for s in /sys/class/drm/$n-*/status; do
            [ -e "$s" ] || continue
            [ "$(cat "$s")" = connected ] && { SCANOUT=$n; break 2; }
        done
    done
    if [ -n "$SCANOUT" ]; then
        DRM_DEVICES=/dev/dri/$GPU_CARD:/dev/dri/$SCANOUT
    else
        echo "gpu_env: no connected monitor on any card" >&2
        DRM_DEVICES=/dev/dri/$GPU_CARD
    fi
fi

case "$GPU_DEV" in
    0x67df) GPU_NAME="RX 570 (gfx803, Polaris)" ;;
    0x687f) GPU_NAME="Vega 56 (gfx900, Vega 10)" ;;
    *)      GPU_NAME="AMD $GPU_PCI" ;;
esac

export GPU_CARD GPU_PCI GPU_NAME DRM_DEVICES GPU_HAS_OUTPUT
