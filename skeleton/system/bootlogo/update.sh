#!/bin/sh

cd $(dirname "$0")

export PATH=$PWD/bin:$PATH
export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH

dd if=/dev/block/by-name/boot of=boot.img

dumpimage -T flat_dt -p 2 -o resource.img boot.img
mkdir resource
cd resource
rsce_tool -u ../resource.img

LOGO_1=../logo.bmp
LOGO_2=../logo_kernel.bmp
[ -f $LOGO_1 ] && cp $LOGO_1 .
[ -f $LOGO_2 ] && cp $LOGO_2 .

rsce_tool \
  -p rk-kernel.dtb \
  -p battery_1.bmp \
  -p battery_2.bmp \
  -p battery_3.bmp \
  -p battery_4.bmp \
  -p battery_5.bmp \
  -p battery_fail.bmp \
  -p logo.bmp \
  -p logo_kernel.bmp \
  -p battery_0.bmp
mv boot-second ../resource-mod.img
cd ../

ORIGINAL_SIZE=$(wc -c < resource.img)
cp resource-mod.img resource-pad.img
truncate -s $ORIGINAL_SIZE resource-pad.img

RESOURCE_OFFSET=$(dtc -I dtb -O dts boot.img |
  sed -n '/resource {/,/};/p' |
  grep data-position |
  sed -E 's/.*<0x([0-9a-fA-F]+)>.*/0x\1/')
dd if=resource-pad.img of=boot.img bs=512 seek=$(($RESOURCE_OFFSET / 512)) conv=notrunc

export OLD_HASH=$(sha256sum resource.img | awk '{print $1}')
export NEW_HASH=$(sha256sum resource-pad.img | awk '{print $1}')
perl -0777 -i -pe '
  $old = pack("H*", $ENV{OLD_HASH});
  $new = pack("H*", $ENV{NEW_HASH});

  $pos = index($_, $old);
  die "old hash not found\n" if $pos < 0;

  substr($_, $pos, length($old)) = $new;
' boot.img

dd if=boot.img of=/dev/block/by-name/boot