# Named virtual NOR boot-command core

The next flash stage starts with a pure command model for a **virtual MX25UM51245GXDI00**. It is not connected to the X-T4 board yet and does not move the established bootstrap checkpoint beyond its first flash write. The camera's actual fitted part remains unknown.

The [Macronix PM2183 v1.6 datasheet](https://www.macronix.com/Lists/Datasheet/Attachments/9241/MX25UM51245G%2C%201.8V%2C%20512Mb%2C%20v1.6.pdf) documents the named part's `C2 80 3A` identification, SPI-start ordering option, write-enable latch, and configuration-register-2 mode selection. The [ST component driver](https://github.com/STMicroelectronics/stm32-mx25um51245g/blob/2fdc96a372222d6a3e47fb84263e176636a68972/mx25um51245g.c) supplies independent protocol examples. Those are facts about the selected virtual part, not recovered camera identity or reset measurements.

The core supports SPI identification, write enable/disable, status and the boot's `72` configuration request; after mode selection it supports complemented octal-DTR write enable/disable and duplicated status reads. It implements no array reads, program, erase, SFDP, media, controller timing, interrupt routing or PHY training. Other commands/framing reject explicitly.

Commands first prepare an effect without changing chip state. Only a later matching commit changes the latch/protocol and advances its generation. A duplicate commit, a competing old effect or an effect prepared before reset rejects without mutation. A configuration write without write-enable is ignored and cannot switch mode. Status bytes follow the latch state. There is no pending program/erase operation or invented WIP pulse in this subset.

The successful volatile configuration effect carries a declared **40 ns typical** settling value from the manufacturer table. The table gives no maximum for that operation. This field does not advance time itself; the future controller must schedule and commit it. It is not a physical-camera timing guarantee.

Seven standalone GLib test groups exercise the real core, including ID prefixes, latch/status transitions, preparation without mutation, ignored configuration, SPI-to-OPI transition, duplicated status, invalid framing and stale/reset effects. They require no firmware, QEMU guest, host media or camera:

```sh
ninja -C build -j4 tests/unit/test-xt4-nor-boot
build/tests/unit/test-xt4-nor-boot
```

The full laboratory test command includes this binary. Controller integration remains the next task; native menus and virtual storage remain unimplemented.
