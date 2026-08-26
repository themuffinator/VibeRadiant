# VibeyMapTools vendor record

This directory is a reduced source snapshot of
[VibeyMapTools](https://github.com/themuffinator/VibeyMapTools), built as
separate command-line programs for VibeRadiant.

- Snapshot: `1028a4f4e8fd7c574488e7e47528e0e4a7b627d3` (verified from
  the local upstream object database; not currently published as an upstream
  ref)
- Upstream description: `v2.1.0-2-g1028a4f4`
- Upstream licence: GNU GPL v3; see [`COPYING`](COPYING)

The uncommitted upstream `src/buildgui` experiment is deliberately excluded:
it was ignored by the upstream repository and therefore has no provenance in
the recorded snapshot. The optional `vmt-hub`, tests, documentation sources,
and release-only assets are also excluded from VibeRadiant's normal tool build.

## Bundled dependencies

| Dependency | Imported version/revision | Licence |
| --- | --- | --- |
| [{fmt}](https://github.com/fmtlib/fmt) | 10.2.1, [`e69e5f977d458f2650bb346dadf2ad30c5320281`](https://github.com/fmtlib/fmt/commit/e69e5f977d458f2650bb346dadf2ad30c5320281) | MIT |
| [JsonCpp](https://github.com/open-source-parsers/jsoncpp) | 1.9.7; recorded import revision `b511701a8` (the stripped vendor snapshot did not retain the full object ID) | Public domain/MIT |
| [ankerl::nanobench](https://github.com/martinus/nanobench) | 4.3.11, [`e4327893194f06928012eb81cabc606c4e4791ac`](https://github.com/martinus/nanobench/commit/e4327893194f06928012eb81cabc606c4e4791ac) | MIT |
| [pareto](https://github.com/alandefreitas/pareto) | 1.2.0, [`47f491eeaead1b5a95e27ee3d6bc4c591b0e4462`](https://github.com/alandefreitas/pareto/commit/47f491eeaead1b5a95e27ee3d6bc4c591b0e4462) | MIT |
| [stb_image / stb_image_write](https://github.com/nothings/stb) | v2.28 / v1.16; files are byte-identical to the recorded VibeyMapTools snapshot | Public domain or MIT |

The dependency licences are compatible with the GPL-3.0-licensed tool
programs. Complete notices and licence-file locations are recorded in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## VibeRadiant integration changes

- JsonCpp's CMake language baseline is raised from C++11 to C++17 to match the
  enclosing build.
- GoogleTest is fetched only when the upstream test suite is enabled.
- MSYS2 Embree and oneTBB licence locations are recognized and their licence
  texts are staged beside the runtime libraries.
- `scripts/build-ericw-tools.sh` provides incremental build/staging integration
  and installs the binaries under VibeRadiant's `qbsp`, `light`, and `vis`
  names.
