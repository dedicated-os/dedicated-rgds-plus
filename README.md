# DEDICATED OS

DEDICATED OS is a cluster of hyper-focused, single-system launchers for specific devices. This version is for playing Nintendo DS on the RG DS Plus. It is also available on the [MagicX Zero40](https://github.com/dedicated-os/dedicated-zero40).

This is what it looks like:

<img width=256 src="github/dedicated-menu.png" title=Menu /> <img width=256 src="github/dedicated-archive.png" title=Archive /> 

That's it. You can [grab the latest version here](https://github.com/dedicated-os/dedicated-rgds/releases).

## Features

- no options
- single save state per game
- fast game switcher
- drastic for emulation
- silent fast forward
- lid support
- mic support
- datetime and timezone switcher
- archive for hiding completed or unstarted games
- automatic save on poweroff and when switching games
- automatic resume on boot and when switching games
- faux sleep with automatic poweroff

## Usage

Press the Anbernic/MENU button to open the in-game menu where you can save, load, and reset the current game or switch to and load or reset a different game. 

Press MENU again to open the system menu where you can manage your archive and set the date, time, and timezone.

<img width=256 src="github/dedicated-datetime.png" title=Datetime /> <img width=256 src="github/dedicated-timezone.png" title=Timezone /> 

Lid events are automatically sent directly to the running game. Games that require the microphone automatically use the microphone.

## Known Issues

Some DS games are known to hang in drastic when the lid is closed (eg. Monster Tale, Radiant Historia, Ni no Kuni). These games are blocked from lid support. They will instead enter or exit faux sleep instead of receiving lid events. If you encounter another game that freezes when the lid is closed, let me know so I can add them to the blocklist.

## Original RG DS Support

The original RG DS is unsupported, its 640x480 screens were a mistake and I won't dignify it with a release.
