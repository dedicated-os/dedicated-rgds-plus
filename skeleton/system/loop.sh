#!/bin/sh

# disable led
led off

# update bootlogo if changed
bootlogo

# drastic
EXEC_LOOP=/tmp/exec_loop
touch $EXEC_LOOP
while [ -f $EXEC_LOOP ]; do
	$SDCARD_PATH/system/run.sh
	sync
done

# reenable led and say goodnight
led on
shutdown
