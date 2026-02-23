Smartiecoin Core
==========

This is the official reference wallet for Smartiecoin digital currency and comprises the backbone of the Smartiecoin peer-to-peer network. You can [download Smartiecoin Core](https://www.smartiecoin.org/downloads/) or [build it yourself](#building) using the guides below.

Running
---------------------
The following are some helpful notes on how to run Smartiecoin Core on your native platform.

### Unix

Unpack the files into a directory and run:

- `bin/smartiecoin-qt` (GUI) or
- `bin/smartiecoind` (headless)

### Windows

Unpack the files into a directory, and then run smartiecoin-qt.exe.

### macOS

Drag Smartiecoin Core to your applications folder, and then run Smartiecoin Core.

### Need Help?

* See the [Smartiecoin documentation](https://docs.smartiecoin.org)
for help and more information.
* Ask for help on [Smartiecoin Discord](http://staydashy.com)
* Ask for help on the [Smartiecoin Forum](https://smartiecoin.org/forum)

Building
---------------------
The following are developer notes on how to build Smartiecoin Core on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [Dependencies](dependencies.md)
- [macOS Build Notes](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [Windows Build Notes](build-windows.md)
- [OpenBSD Build Notes](build-openbsd.md)
- [NetBSD Build Notes](build-netbsd.md)
- [Android Build Notes](build-android.md)

Development
---------------------
The Smartiecoin Core repo's [root README](/README.md) contains relevant information on the development process and automated testing.

- [Developer Notes](developer-notes.md)
- [Productivity Notes](productivity.md)
- [Release Notes](release-notes.md)
- [Release Process](release-process.md)
- Source Code Documentation ***TODO***
- [Translation Process](translation_process.md)
- [Translation Strings Policy](translation_strings_policy.md)
- [JSON-RPC Interface](JSON-RPC-interface.md)
- [Unauthenticated REST Interface](REST-interface.md)
- [Shared Libraries](shared-libraries.md)
- [BIPS](bips.md)
- [Dnsseed Policy](dnsseed-policy.md)
- [Benchmarking](benchmarking.md)
- [Internal Design Docs](design/)

### Resources
* See the [Smartiecoin Developer Documentation](https://dashcore.readme.io/)
  for technical specifications and implementation details.
* Discuss on the [Smartiecoin Forum](https://smartiecoin.org/forum), in the Development & Technical Discussion board.
* Discuss on [Smartiecoin Discord](http://staydashy.com)
* Discuss on [Smartiecoin Developers Discord](http://chat.dashdevs.org/)

### Miscellaneous
- [Assets Attribution](assets-attribution.md)
- [smartiecoin.conf Configuration File](smartiecoin-conf.md)
- [CJDNS Support](cjdns.md)
- [Files](files.md)
- [Fuzz-testing](fuzzing.md)
- [I2P Support](i2p.md)
- [Init Scripts (systemd/upstart/openrc)](init.md)
- [Managing Wallets](managing-wallets.md)
- [Multisig Tutorial](multisig-tutorial.md)
- [P2P bad ports definition and list](p2p-bad-ports.md)
- [PSBT support](psbt.md)
- [Reduce Memory](reduce-memory.md)
- [Reduce Traffic](reduce-traffic.md)
- [Tor Support](tor.md)
- [Transaction Relay Policy](policy/README.md)
- [ZMQ](zmq.md)

License
---------------------
Distributed under the [MIT software license](/COPYING).
