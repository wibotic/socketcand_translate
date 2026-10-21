# Third-party versions

Versions of the OpenCyphal code and toolchain used by this firmware.
Update this file whenever one of them changes. Last checked: 2026-10-21.

## OpenCyphal

| Component | Version | Source | How it is included |
|---|---|---|---|
| libcanard | 5.0.0 | ESP Component Registry `opencyphal/libcanard` (upstream: <https://github.com/OpenCyphal/libcanard>, tag `v5.0.0`) | `main/idf_component.yml`; downloaded into `managed_components/` at build time (not committed) |
| o1heap | 3.0.0 | <https://github.com/pavel-kirienko/o1heap>, tag `3.0.0` | Vendored in `components/esp_o1heap/` (`o1heap.c`, `include/o1heap.h`), unmodified. Not on the ESP registry |
| Nunavut (DSDL generator) | 2.3.1 | PyPI `nunavut` | Run on a PC by `components/esp_nunavut/recompile_dsdl.sh`; not part of the firmware |
| DSDL types | `public_regulated_data_types` master as of 2024-09-11 (exact commit not recorded) | <https://github.com/OpenCyphal/public_regulated_data_types> | Generated C headers in `components/esp_nunavut/nunavut/`. The firmware only uses `uavcan.node.Heartbeat.1.0`, `Health.1.0` and `Mode.1.0` |
| Cyphal specification | 1.0 | <https://opencyphal.org> | n/a |

### Known issue: libcanard 5.0.0 clashes with ESP-IDF's `random()`

`canard.c` defines `static uint64_t random(canard_t*, uint64_t)`, which clashes with
`long random(void)` declared by ESP-IDF's newlib `stdlib.h`, so a plain build fails with
`error: conflicting types for 'random'`. Upstream master has the same code and there is
no 5.0.1 yet. (The earlier missing `cavl2.h` problem of 4.0.0 is fixed in 5.0.0.)

Workaround in this repo, without editing the downloaded library: `main/CMakeLists.txt`
defines `CANARD_CONFIG_HEADER` for the libcanard component, so `canard.c` includes
`main/canard_config.h`, which does `#define random canard_random` after the standard
headers and before libcanard's definition. A clean build (`idf.py fullclean`, then
`idf.py build`) works with the unmodified registry package.

When a fixed release exists: raise the requirement in `main/idf_component.yml` to that
release, delete `main/canard_config.h` and the `CANARD_CONFIG_HEADER` lines in
`main/CMakeLists.txt`, rebuild from clean, and delete this section.

## Toolchain

| Component | Version |
|---|---|
| ESP-IDF | v5.3.1 in CI (`.github/workflows/build.yml` and `release.yml`). `sdkconfig.defaults` was saved with 5.2.2. Stay compatible with 5.3.x |
| `espressif/button` | `^3.3.0` (`main/idf_component.yml`) |

## Updating

- **libcanard**: change the version requirement in `main/idf_component.yml`, run
  `idf.py update-dependencies`, then build. A new major version changes the C API
  and needs edits in `main/cyphal_node.c` (v4 to v5 replaced the whole API).
- **o1heap**: copy `o1heap/o1heap.c` and `o1heap/o1heap.h` from the new upstream
  tag over the two vendored files. Keep the mutex in `main/cyphal_node.c`; the
  heap is not thread-safe.
- **DSDL headers**: run `components/esp_nunavut/recompile_dsdl.sh` (it downloads
  unpinned `master` and the newest Nunavut, so record the result here).

After any update: `idf.py build` must pass with no warnings, then test on
hardware (heartbeats visible with `yakut monitor`, `/api/status` counters rising).

To check that a vendored file is an unmodified upstream release, `diff` it
against the same file from a clone checked out at the tag:

```bash
git -C o1heap show 3.0.0:o1heap/o1heap.c | diff - components/esp_o1heap/o1heap.c
```
