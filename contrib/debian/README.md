
Debian
====================
This directory contains files used to package smartiecoind/smartiecoin-qt
for Debian-based Linux systems. If you compile smartiecoind/smartiecoin-qt yourself, there are some useful files here.

## smartiecoin: URI support ##


smartiecoin-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install smartiecoin-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your smartiecoin-qt binary to `/usr/bin`
and the `../../share/pixmaps/smartiecoin128.png` to `/usr/share/pixmaps`

smartiecoin-qt.protocol (KDE)

