# VibeyMapTools vendor record

This directory is a reduced source snapshot of
[VibeyMapTools](https://github.com/themuffinator/VibeyMapTools), built as
separate command-line programs for VibeRadiant.

- Snapshot: `4495049a9e4c1f6deadae3a76b8256614840af35` (verified from
  the clean local upstream checkout)
- Upstream description: `v3.1.0-1-g4495049a`
- Upstream licence: GNU GPL v3; see [`COPYING`](COPYING)

The ignored upstream `src/buildgui` experiment is deliberately excluded:
it was ignored by the upstream repository and therefore has no provenance in
the recorded snapshot. The optional `vmt-hub` remains available but is disabled
by default. Upstream tests, documentation sources, and release-only assets are
not bundled; attempts to enable the absent test or documentation trees stop at
configure time with a clear diagnostic.

The bundled dependencies are production-source subsets. The {fmt}
documentation and tests; JsonCpp documentation, tests, examples, and test
runners; and pareto documentation, tests, examples, and Python bindings are
excluded. Nanobench is omitted entirely because no production target links it.

## Bundled dependencies

| Dependency | Imported version/revision | Licence |
| --- | --- | --- |
| [{fmt}](https://github.com/fmtlib/fmt) | 12.1.0, [`407c905e45ad75fc29bf0f9bb7c5c2fd3475976f`](https://github.com/fmtlib/fmt/commit/407c905e45ad75fc29bf0f9bb7c5c2fd3475976f) | MIT |
| [JsonCpp](https://github.com/open-source-parsers/jsoncpp) | 1.9.6, [`89e2973c754a9c02a49974d839779b151e95afd6`](https://github.com/open-source-parsers/jsoncpp/commit/89e2973c754a9c02a49974d839779b151e95afd6) | Public domain/MIT |
| [pareto](https://github.com/alandefreitas/pareto) | 1.2.0, [`47f491eeaead1b5a95e27ee3d6bc4c591b0e4462`](https://github.com/alandefreitas/pareto/commit/47f491eeaead1b5a95e27ee3d6bc4c591b0e4462) | MIT |
| [stb_image / stb_image_write](https://github.com/nothings/stb) | stb_image v2.28 with the [v2.30 GCC warning fix](https://github.com/nothings/stb/blob/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h#L4804-L4809) backported / stb_image_write v1.16 | Public domain or MIT |

The dependency licences are compatible with the GPL-3.0-licensed tool
programs. Complete notices and licence-file locations are recorded in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## VibeRadiant integration changes

- JsonCpp's CMake language baseline is raised from C++11 to C++17 to match the
  enclosing build, and its minimum policy version is raised to CMake 3.10 to
  match the enclosing project's newer baseline.
- All command-line tools accept `-version` and `--version` as successful
  version-only invocations.
- The absent upstream tests and documentation stay disabled by default and
  produce a clear configure-time error if requested.
- MSYS2 Embree and oneTBB licence locations are recognized and their licence
  texts are staged beside the runtime libraries.
- `scripts/build-ericw-tools.sh` provides incremental build/staging integration
  and installs the binaries under VibeRadiant's `qbsp`, `light`, and `vis`
  names.
