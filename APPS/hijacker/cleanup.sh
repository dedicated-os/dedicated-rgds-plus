#!/bin/bash

# copy to /tmp before running
case "$0" in
  /tmp/*)
    # already running from /tmp, just continue
    ;;
  *)
    TMP_COPY="/tmp/$(basename "$0")"
    cp "$0" "$TMP_COPY"
    chmod +x "$TMP_COPY"
    exec "$TMP_COPY" "$@"
    ;;
esac

# clean up stock detritus
find /mnt/mmc -mindepth 1 -exec rm -rf -- {} +

# shutdown
echo s > /proc/sysrq-trigger
echo u > /proc/sysrq-trigger
VALUE=$(i2cget -f -y 0 0x20 0xf4)
i2cset -f -y 0 0x20 0xf4 $((VALUE | 1))