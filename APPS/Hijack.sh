#/bin/sh

cd $(dirname "$0")

mv /etc/init.d/S49weston /etc/init.d/disabled.S49weston
mv /etc/init.d/S50launch /etc/init.d/disabled.S50launch

# TODO: test, was cp then chmod +x for each
install -m 755 hijacker/S01batmon /etc/init.d/
install -m 755 hijacker/S50hijack /etc/init.d/

hijacker/cleanup.sh