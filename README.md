# rtss-mailbox-kmd

RTSS Mailbox Kernel Mode Driver (KMD) for IPC-based inter-processor
communication on Qualcomm® Automotive, IE-IoT, and Robotics SoCs.

Implements the `/dev/rtssmb` character device that bridges Linux userspace and
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
- Qualcomm SoC with RTSS: qcs9100 / qcs9075 / qcs8300 / qcs8275 (see
  `meta-qcom/conf/machine/` for the corresponding Yocto MACHINE targets,
  e.g. `iq-9075-evk`, `qcs9100-ride-sx`, `iq-8275-evk`, `qcs8300-ride-sx`)
- Cross-compiler: `aarch64-qcom-linux-gcc`

## Device Tree

The RTSS mailbox node is defined in the shared staging overlays (these
overlays also carry other nodes as well, not just RTSS mailbox).

```
arch/arm64/boot/dts/qcom/lemans-staging.dtso    (Lemans family, qcom,sa8775p-rtss-mailbox)
arch/arm64/boot/dts/qcom/monaco-staging.dtso    (Monaco family, qcom,qcs8300-rtss-mailbox)
```

## Build Instructions

### Yocto (recommended)

```bash
source <poky>/oe-init-build-env <build-dir>
MACHINE=iq-9075-evk bitbake qcom-rtss-mailbox-dlkm
```

`iq-9075-evk` is shown as the example; `iq-8275-evk`, `qcs9100-ride-sx`,
and `qcs8300-ride-sx` are also valid `MACHINE` targets (see Requirements
above).

Packages produced:

| Package | Contents |
|---------|----------|
| `qcom-rtss-mailbox-dlkm` | `rtss_mailbox.ko` DLKM |
| `qcom-rtss-mailbox-uapi-headers` | `rtss_mailbox_uapi.h` for UMD build (standalone recipe) |

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
make headers_install INSTALL_HDR_PATH=<target-dir>
```

This installs `include/uapi/rtss_mailbox_uapi.h` to `<target-dir>/include/`.
`INSTALL_HDR_PATH` defaults to `usr` (i.e. `usr/include/`) if not set.

## Usage

Load the DLKM (requires the `qcom_ipcc` mailbox controller to be present
first). `osal/Kconfig` enforces `depends on MAILBOX && QCOM_IPCC`, so
`CONFIG_QCOM_RTSS_MAILBOX` cannot even be configured without them. On all
currently supported targets `QCOM_IPCC` is built in (`CONFIG_QCOM_IPCC=y`),
so loading is normally automatic. If a kernel instead builds it as a
loadable module, note there is no `MODULE_SOFTDEP` on it, so it must be
`modprobe`d manually before this driver):

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

The KMD exposes a single `/dev/rtssmb` character device. It handles:

- **IPC interrupt registration** — per-channel eventfd signalling to userspace
- **Shared-memory carveout mapping** — `mmap()` of mailbox and OTA DDR regions
- **PM suspend/resume** — RTSS handshake coordination across power state transitions

Middleware and driver architecture improvements are planned. Existing
interfaces will be maintained; applications may migrate to new interfaces
as they become available.

## UAPI Version

**This driver is still being upstreamed. The uAPI is not yet frozen — struct
layout and IOCTL numbers may change before the driver is accepted upstream.
Do not treat the current layout as a stable ABI.**

The driver enforces a UAPI version check on every IOCTL call:

```c
/* include/uapi/rtss_mailbox_uapi.h */
#define RTSS_MB_UAPI_VERSION_MAJOR  1
#define RTSS_MB_UAPI_VERSION_MINOR  0
```

A major version mismatch between KMD and UMD is rejected with `-EINVAL`.
Minor version increments are backward compatible.

### Future uAPI Direction (Subject to Upstream Review)

The current uAPI is IOCTL-based. As this driver moves toward upstream
acceptance, the intended direction is to migrate `/dev/rtssmb` towards
standard POSIX file operations (`open`/`read`/`write`/`poll`/`close`, with
`ioctl()` retained for control/metadata operations that have no stream-op
equivalent), with each channel exposed via its own device interface
(`/dev/rtss/<endpoint>`).

This is a proposed direction, not a committed change — the current
IOCTL-based uAPI remains in place and fully supported until any such
migration is reviewed and accepted upstream. See the companion
`rtss-mailbox-umd` README's "Future uAPI Direction (Subject to Upstream
Review)" section for the detailed current-API-to-POSIX mapping.

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
