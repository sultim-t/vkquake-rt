# Quake: Ray Traced

Quake: Ray Traced adds a path tracing renderer to id Software's [Quake](https://en.wikipedia.org/wiki/Quake_(video_game)) using the [RayTracedGL1](https://github.com/sultim-t/RayTracedGL1) library.

Quake: Ray Traced is based on the [vkQuake](https://github.com/Novum/vkQuake) — a port of QuakeSpasm to Vulkan API.

## Build

### Windows

Clone the vkQuake repo from `https://github.com/sultim-t/vkquake-rt.git`

Prerequisites:

* [Git for Windows](https://github.com/git-for-windows/git/releases)
* GPU with a ray tracing support

Steps:

* Install [Visual Studio Community](https://www.visualstudio.com/products/free-developer-offers-vs) with Visual C++ component
* Open the Visual Studio solution, `Windows\VisualStudio\vkquake.sln`
* Build the [RayTracedGL1](https://github.com/sultim-t/RayTracedGL1/tree/quake) library, provide its header files / `.lib` to the `vkQuake` solution
* Build the `vkQuake` solution
* Copy `RayTracedGL1.dll` next to `vkQuake.exe` before running the executable

### macOS (experimental, Apple Silicon)

Runs natively through [MoltenVK](https://github.com/KhronosGroup/MoltenVK) with its experimental ray tracing support ([MoltenVK PR #2771](https://github.com/KhronosGroup/MoltenVK/pull/2771), not merged yet). Apple MetalFX replaces AMD FSR 2.0 as the upscaler.

Prerequisites:

* Xcode 27 or newer (macOS 27 SDK), CMake, Ninja
* MoltenVK built from PR #2771, SDL2, and optionally libogg / libvorbis for music
* [RayTracedGL1](https://github.com/sultim-t/RayTracedGL1/tree/quake) `quake` branch with macOS support, built with `-DRG_WITH_SURFACE_METAL=ON -DRG_WITH_AMD_FSR2=OFF` against MoltenVK; its shaders compiled with glslang (`--target-env vulkan1.2`) into `ovrd/shaders`

Steps:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DRTGL1_SDK_PATH=/path/to/RayTracedGL1 \
      -DRTGL1_LIBRARY=/path/to/libRayTracedGL1.dylib \
      -DQRT_WITH_VORBIS=ON
ninja -C build
```

Run with `MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=1` set, and the game data (`id1`, Quake RT's `ovrd`) in the folder passed as `-basedir`.
