# libpsu Portable Library

Portable PMBus protocol library for power supply units. Provides register
definitions, status bit masks, and data format conversions (LINEAR11/LINEAR16)
that work across Linux kernel modules and bare-metal firmware.

## Features

- PMBus command codes, status flags, and format constants
- LINEAR11 / LINEAR16 conversion functions (milli-unit precision)
- Vendor definitions for Chicony S24 and BOCO BS402A17/BS502A17
- Per-command PEC (Packet Error Checking) policy per vendor
- Dual licensed: Apache-2.0 OR GPL-2.0-only

## Building

Requires [Hermit](https://cashapp.github.io/hermit/). From repository root,
activate the environment and build the `lib/` project:

```sh
. ./bin/activate-hermit
meson setup lib/builddir lib
meson compile -C lib/builddir
meson test -C lib/builddir --print-errorlogs
```

## Usage

Add libpsu as a Meson subproject or include the headers directly. The library
has no external dependencies and uses only portable C11.

```c
#include <libpsu/pmbus_protocol.h>
#include <libpsu/pmbus_conversion.h>
#include <libpsu/vendors/chicony.h>

// Convert a raw LINEAR11 register value to millivolts.
int32_t mv = psu_linear11_to_milli(raw);
```

For kernel module integration, use the provided `Kbuild` file.

## Documentation

- [SPEC.md](SPEC.md) — implementation details and vendor quirks
- [DIAGRAMS.md](DIAGRAMS.md) — architecture and flow diagrams

## License

Apache-2.0 OR GPL-2.0-only, at your option. See [../LICENSE](../LICENSE) for
Apache-2.0 and [../LICENSES/GPL-2.0-only.txt](../LICENSES/GPL-2.0-only.txt) for
GPL-2.0-only. Kernel builds use the GPL alternative.
