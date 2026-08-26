# Third-party notices

VibeyMapTools includes the following third-party source dependencies. Their
licence texts are retained in the source tree and copied into VibeRadiant's
staged `ericw` directory under the names shown here.

| Component | Copyright/licence | Source licence | Staged filename |
| --- | --- | --- | --- |
| [{fmt}](https://github.com/fmtlib/fmt) | Copyright Victor Zverovich and contributors; MIT | `extern/fmt/LICENSE` | `LICENSE-fmt.txt` |
| [JsonCpp](https://github.com/open-source-parsers/jsoncpp) | Copyright Baptiste Lepilleur and the JsonCpp Authors; public domain/MIT | `extern/jsoncpp/LICENSE` | `LICENSE-jsoncpp.txt` |
| [pareto](https://github.com/alandefreitas/pareto) | Copyright Alan de Freitas; MIT | `extern/pareto/LICENSE` | `LICENSE-pareto.txt` |

`extern/stb_image.h` and `extern/stb_image_write.h`, by Sean Barrett and other
contributors, are available under either the public-domain dedication or the
MIT licence embedded at the end of each header. Those source files are
byte-identical to the files in the recorded VibeyMapTools snapshot.

[Embree](https://github.com/RenderKit/embree) (Apache-2.0) and
[oneTBB](https://github.com/uxlfoundation/oneTBB) (Apache-2.0) are external
runtime dependencies rather than vendored source. When those libraries are
packaged, the build copies their installed licence texts alongside them as
`LICENSE-embree.txt` and `LICENSE-oneTBB.txt`.
