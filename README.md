# rtss-mailbox-kmd

RTSS Mailbox Kernel Mode Driver (KMD) for IPC-based inter-processor
communication on Qualcomm® Automotive, IE-IoT, and Robotics SoCs.

Implements the `/dev/rtssmb` misc device that bridges Linux userspace and
RTSS (Real-Time SubSystem) over the Qualcomm IPC Controller.
The driver manages the IPC interrupt path and maps the shared-memory
carveout for the companion userspace package
(`rtss-mailbox-umd`).

## Branches

| Branch | Purpose |
|--------|---------|
| `main` | Primary development branch. All contributions target this branch. |
| `rtss-mailbox-kernel.le.0.0` | LE product release branch. Tracks validated releases for Qualcomm LE platforms. |

## Components

| Component | Path | Description |
|-----------|------|-------------|
| Driver source | `osal/rtss_mailbox.c` | Platform driver implementing `/dev/rtssmb` |
| Kconfig | `osal/Kconfig` | `CONFIG_QCOM_RTSS_MAILBOX` build option |
| UAPI header | `include/uapi/rtss_mailbox_uapi.h` | IOCTL definitions shared with userspace |
| Compat header | `include/rtss_mailbox_compat.h` | Kernel API wrappers for multi-version support |

## Requirements

- Linux kernel source tree (6.6+ recommended; compat shims cover 6.4–6.11+ API changes)
- Qualcomm SoC with RTSS: qcs9100 / qcs9075 / qcs8300 / qcs8275
- Cross-compiler: `aarch64-qcom-linux-gcc`

## Device Tree

Device tree overlays for supported targets are available in the
QLI kernel tree:

```
arch/arm64/boot/dts/qcom/lemans-rtss-mb.dtso
arch/arm64/boot/dts/qcom/monaco-rtss-mb.dtso
```

## Build Instructions

### Yocto (recommended)

```bash
source <poky>/oe-init-build-env <build-dir>
MACHINE=qcs9100 bitbake qcom-rtss-mailbox-kmd
```

Packages produced:

| Package | Contents |
|---------|----------|
| `qcom-rtss-mailbox-kmd` | `rtss_mailbox.ko` DLKM |
| `qcom-rtss-mailbox-kmd-dev` | `rtss_mailbox_uapi.h` for UMD build |

### Standalone (DLKM out-of-tree build)

```bash
export KERNEL_SRC=<path to kernel build directory>
make ARCH=arm64 CROSS_COMPILE=aarch64-qcom-linux-
```

Install the DLKM:

```bash
make modules_install
```

Export the UAPI header for userspace builds:

```bash
make headers_install
```

## Usage

Load the DLKM (respects the `qcom_ipcc` soft dependency):

```bash
modprobe rtss_mailbox
```

Verify the device node is created:

```bash
ls -la /dev/rtssmb
```

The driver is used exclusively through the `rtss-mailbox-umd` userspace
library. Direct IOCTL access is not recommended — use the `rtss_mb_*` API
from `rtss-mailbox-umd` instead.

## Architecture

The KMD exposes a single `/dev/rtssmb` misc device. It handles:

- **IPC interrupt registration** — per-channel eventfd signalling to userspace
- **Shared-memory carveout mapping** — `mmap()` of mailbox and OTA DDR regions
- **PM suspend/resume** — RTSS handshake coordination across power state transitions

Middleware and driver architecture improvements are planned. Existing
interfaces will be maintained; applications may migrate to new interfaces
as they become available.

## UAPI Version

The driver enforces a UAPI version check on every IOCTL call:

```c
/* include/uapi/rtss_mailbox_uapi.h */
#define RTSS_MB_UAPI_VERSION_MAJOR  1
#define RTSS_MB_UAPI_VERSION_MINOR  0
```

A major version mismatch between KMD and UMD is rejected with `-EINVAL`.
Minor version increments are backward compatible.

## Kernel Compatibility

Cross-version API differences are handled in `include/rtss_mailbox_compat.h`.
Driver code uses only the `rtssmb_*` wrappers — never calls the kernel APIs
directly. Add a new entry there whenever an upstream API signature changes.

| Wrapper / Macro | Change | Boundary |
|---|---|---|
| `rtssmb_class_create(name)` | `class_create()` lost `THIS_MODULE` arg | kernel 6.4 |
| `rtssmb_eventfd_signal(ctx)` | `eventfd_signal()` lost count arg | kernel 6.8 |
| `RTSSMB_REMOVE_RETURN_TYPE` / `RTSSMB_REMOVE_RETURN` | `platform_driver.remove` return type changed `int` → `void` | kernel 6.11 |

## Development

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to submit patches and pull requests.

Coding style follows the Linux kernel C style consistent with in-tree Qualcomm
platform drivers.

## Getting in Contact

- [Report an Issue on GitHub](../../issues)
- [Open a Discussion on GitHub](../../discussions)
- [Security issues](mailto:product-security@qualcomm.com)

## License

`rtss-mailbox-kmd` is licensed under [GPL-2.0-only](https://spdx.org/licenses/GPL-2.0-only.html).
See [LICENSE.txt](LICENSE.txt) for the full license text.

The UAPI header (`include/uapi/rtss_mailbox_uapi.h`) carries
`GPL-2.0-only WITH Linux-syscall-note` allowing userspace applications to
include it without GPL license requirements.
