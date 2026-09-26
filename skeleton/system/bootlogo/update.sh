#!/bin/sh

cd $(dirname "$0")

export PATH=$PWD/bin:$PATH
export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH

dd if=/dev/block/by-name/boot of=boot.img

dumpimage -T flat_dt -p 2 -o resource.img boot.img
rm -rf resource
mkdir resource
cd resource

rsce_tool -u ../resource.img | tee ../resource-files.txt

LOGO_1=../logo.bmp
LOGO_2=../logo_kernel.bmp
[ -f $LOGO_1 ] && cp $LOGO_1 .
[ -f $LOGO_2 ] && cp $LOGO_2 .

ESC=$(printf '\033')
FILES=$(sed -n 's/^.*Creating files://p' ../resource-files.txt | sed "s/${ESC}\\[[0-9;]*m//g")

rsce_tool $(for FILE in $FILES; do printf ' -p %s' "$FILE"; done)
mv boot-second ../resource-mod.img
cd ../

RESOURCE_OFFSET=0x$(fdtget -t x boot.img /images/resource data-position)
ORIGINAL_SIZE=0x$(fdtget -t x boot.img /images/resource data-size)
ORIGINAL_TOTAL=0x$(fdtget -t x boot.img / totalsize)
RESOURCE_OFFSET=$(($RESOURCE_OFFSET))
ORIGINAL_SIZE=$(($ORIGINAL_SIZE))
ORIGINAL_TOTAL=$(($ORIGINAL_TOTAL))
RESOURCE_SIZE=$(wc -c < resource-mod.img)
RESOURCE_END=$(($RESOURCE_OFFSET + $RESOURCE_SIZE))
TOTAL_SIZE=$(($RESOURCE_END + $ORIGINAL_TOTAL - $RESOURCE_OFFSET - $ORIGINAL_SIZE))

dd if=resource-mod.img of=boot.img bs=512 seek=$(($RESOURCE_OFFSET / 512)) conv=notrunc

export ORIGINAL_SIZE RESOURCE_SIZE ORIGINAL_TOTAL TOTAL_SIZE
HASH=$(sha256sum resource-mod.img | awk '{print $1}')
export OLD_HASH=$(sha256sum resource.img | awk '{print $1}')
export NEW_HASH=$HASH
perl -0777 -i -pe '
  BEGIN {
    $old_size = pack("N", $ENV{ORIGINAL_SIZE});
    $new_size = pack("N", $ENV{RESOURCE_SIZE});
    $old_total = pack("N", $ENV{ORIGINAL_TOTAL});
    $new_total = pack("N", $ENV{TOTAL_SIZE});
    $old_hash = pack("H*", $ENV{OLD_HASH});
    $new_hash = pack("H*", $ENV{NEW_HASH});
  }

  s/\Q$old_size\E/$new_size/;
  s/\Q$old_total\E/$new_total/;
  s/\Q$old_hash\E/$new_hash/;
' boot.img

dd if=boot.img of=/dev/block/by-name/boot
