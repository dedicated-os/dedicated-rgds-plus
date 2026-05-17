#/bin/sh

cd $(dirname "$0")

mv /etc/init.d/S49weston /etc/init.d/disabled.S49weston
mv /etc/init.d/S50launch /etc/init.d/disabled.S50launch
cp hijacked/S50hijack /etc/init.d

hijacked/cleanup.sh