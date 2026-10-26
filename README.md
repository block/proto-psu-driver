# libpsu

This repository contains two related components:

1. `lib/` - portable PMBus library code shared across kernel and firmware consumers.
2. `psu-pmbus-driver/` - Linux kernel module source for the PSU PMBus driver.

## Portable Library (`lib/`)

Build and test with Meson:

```sh
. ./bin/activate-hermit
meson setup lib/builddir lib
meson compile -C lib/builddir
meson test -C lib/builddir --print-errorlogs
```

See [lib/README.md](lib/README.md) for API and design details.

## Kernel Driver (`psu-pmbus-driver/`)

The Yocto recipe in miner-firmware fetches this repository and builds the module from `psu-pmbus-driver/`.

Manual build example:

```sh
cd psu-pmbus-driver
make KERNEL_SRC=/path/to/kernel/build
```

The driver `Makefile` links shared conversion logic from `../lib/src/pmbus_conversion.c` and includes headers from `../lib/include`.

## License

The kernel driver and shared portable library are dual licensed under
Apache-2.0 OR GPL-2.0-only, at your option. Other repository content is
Apache-2.0 unless marked otherwise. Copyright notices remain unchanged.

See [LICENSE](LICENSE) for Apache-2.0 and
[LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt) for GPL-2.0-only.

The GPL license text is copied verbatim from [GNU](https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt).

`script/copybara.py` also stages this Apache license as the destination's root
`LICENSE`, without creating a commit.
The exporter also removes and stages destination-tracked files that are absent
from the source export, including previously exported tooling now excluded.
The generated root `LICENSE`, unrelated submodules, and ignored local files are
preserved. Ordinary untracked files still cause preflight to reject the
destination. Source paths still in the index but missing from the working tree
are rejected; stage source deletions with `git rm` before exporting.
Transferred regular files retain their source-index executable modes in both
the destination index and working tree, regardless of `core.fileMode`.
Unrelated destination submodules are left untouched; exports reject paths
inside them before making changes. Open-source exports omit the transfer
script, its regression tests, and local Raspberry Pi build tooling.
These tooling exclusions also apply when the exporter runs from another checkout.

When changes are staged, the exporter prints the expected commit message:

```text
Sync proto-distro source snapshot

Source-ref: refs/heads/main
Source-commit: <source HEAD commit SHA>
```

The ref and SHA come from the source repository, not the destination.
Detached sources use `Source-ref: HEAD`. A source with no commits reports
that no commit message is available. The exporter does not create a commit.
If tracked source files have uncommitted changes, a warning explains that
`Source-commit` identifies the HEAD baseline, not the exact exported contents.

Kernel builds select the GPL alternative and retain `MODULE_LICENSE("GPL")`
for access to GPL-only kernel APIs. Existing `EXPORT_SYMBOL_GPL` restrictions
remain unchanged.
