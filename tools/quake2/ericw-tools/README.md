<div align="center">

<img src="assets/img/banner_2x1.png" alt="VibeyMapTools Banner" width="800">

# VibeyMapTools (VMT)

**Modern BSP compilation tools for Quake engine mapping**

Formerly ericw-tools, now with extra sparkle ✨🎧🛠️

[![Build Status](https://img.shields.io/github/actions/workflow/status/themuffinator/VibeyMapTools/ci.yml?branch=main&style=flat-square)](../../actions)
[![Release](https://img.shields.io/github/v/release/themuffinator/VibeyMapTools?style=flat-square)](../../releases)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue?style=flat-square)](COPYING)

[**Download**](../../releases) | [**Documentation**](docs/) | [**Wiki**](WIKI.md) | [**Changelog**](CHANGELOG.md)

</div>

---

## 🧰 Tools (aka the toolbox)

<table>
<tr>
<td width="120"><b>vmt-bsp</b></td>
<td>Compile .map to .bsp</td>
</tr>
<tr>
<td><b>vmt-vis</b></td>
<td>Calculate PVS visibility</td>
</tr>
<tr>
<td><b>vmt-light</b></td>
<td>Calculate lightmaps with GPU acceleration</td>
</tr>
<tr>
<td><b>vmt-bspinfo</b></td>
<td>BSP information utility</td>
</tr>
<tr>
<td><b>vmt-bsputil</b></td>
<td>BSP manipulation utility</td>
</tr>
<tr>
<td><b>vmt-maputil</b></td>
<td>Lua scripting for .map workflows</td>
</tr>
<tr>
<td><b>vmt-hub</b></td>
<td>Map build hub (Qt)</td>
</tr>
</table>

## 🔁 Coming from ericw-tools? Here is the glow-up 💡🧥✨

Same bones, new polish. If you used `qbsp/vis/light` before, your muscle memory still works. Flags and map formats are familiar. VMT adds modern build + packaging and a pile of shiny upgrades. 🧃🛠️

**Rename cheat sheet (old -> new):**

| ericw-tools | VibeyMapTools |
|-------------|---------------|
| `qbsp` | `vmt-bsp` |
| `vis` | `vmt-vis` |
| `light` | `vmt-light` |
| `bspinfo` | `vmt-bspinfo` |
| `bsputil` | `vmt-bsputil` |
| `maputil` | `vmt-maputil` |
| `hub` | `vmt-hub` |

**What stayed the same (cozy mode):**
- Classic compile flow: `bsp -> vis -> light`
- Quake II + Remaster support is still here (`-q2bsp`, `-lightgrid`, `-world_units_per_luxel`) 🧠🎮
- CLI flag vocabulary, map formats, and output expectations
- Upstream docs still apply: [ericw-tools docs](https://ericwa.github.io/ericw-tools/) 📚

**What is new & shiny (and yes it is a lot):**
- `vmt-` prefix on every binary so ericw-tools can live side-by-side 🧹✨
- Semantic versioning + tag-driven releases with CI-built packages 📦🚀
- Build metadata + `version.hh` for scripting and diagnostics 🧾🛠️
- Improved Half-Life support (updated behavior + smoother compatibility) ??
- Numerous bug fixes and improvements across the toolchain ?????


## 🚀 Quick Start

```bash
# Standard compilation (classic flow ✨)
vmt-bsp mymap.map
vmt-vis mymap.bsp
vmt-light mymap.bsp

# High-quality with GPU acceleration (laser bounces 🔥)
vmt-light -gpu -extra4 -bounce 2 -denoise mymap.bsp
```

## 🌟 Key Features

- 🧠 GPU raytracing via OptiX (`-gpu`) for modern lighting speed
- 🤖 AI denoising with Intel OIDN (`-denoise`)
- 🎛️ Stochastic sampling (`-stochastic`) for complex scenes
- 🔁 Incremental lighting (`-incremental`) for fast iteration loops
- 🌈 HDR lightmaps (`-hdr`) for engines that support it

## 📦 Downloads

| Platform | Package |
|:--------:|---------|
| <img src="https://cdn.simpleicons.org/windows" width="16"> **Windows** | `vibeymaptools-windows-*.zip` |
| <img src="https://cdn.simpleicons.org/linux" width="16"> **Linux** | `vibeymaptools-linux-*.tar.gz` |
| <img src="https://cdn.simpleicons.org/apple" width="16"> **macOS** | `vibeymaptools-macos-*.tar.gz` |

✨ [**Latest Release**](../../releases/latest)

## 🧪 Building from Source

```bash
git clone --recursive https://github.com/themuffinator/VibeyMapTools.git
cd VibeyMapTools && cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

macOS 10.15+ prep: `brew install cmake embree tbb qt@6` (Qt6 only needed for `vmt-hub`)

<details>
<summary><b>Requirements</b></summary>

- CMake 3.14+
- C++20 compiler (MSVC 2019+, GCC 9+, Clang 10+)
- macOS 10.15+ (Catalina) recommended for Apple builds
- Embree 4.x + oneTBB (required for `vmt-light`)

**Optional extras:**
- Qt6 (for `vmt-hub`)
- CUDA + OptiX SDK (GPU raytracing)
- Intel OIDN (AI denoising)

</details>

See [**BUILDING.md**](BUILDING.md) for detailed instructions.

## 📚 Documentation

| Document | Description |
|----------|-------------|
| [BUILDING.md](BUILDING.md) | Build instructions, versioning, CI/CD |
| [CHANGELOG.md](CHANGELOG.md) | Version history and release notes |
| [WIKI.md](WIKI.md) | Documentation hub and quick reference |
| [docs/](docs/) | Full tutorials and reference |

## 🧾 License

GPL v3 - See [COPYING](COPYING)

<div align="center">
<sub>
Based on <a href="https://github.com/ericwa/ericw-tools">ericw-tools</a> by Eric Wasylishen
</sub>
</div>


