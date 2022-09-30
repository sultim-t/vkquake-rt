# Quake: Ray Traced [![Windows CI](https://github.com/sultim-t/vkquake-rt/actions/workflows/build-windows.yml/badge.svg)](https://github.com/sultim-t/vkquake-rt/actions/workflows/build-windows.yml)

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
* Build the [RayTracedGL1](https://github.com/sultim-t/RayTracedGL1) library, provide its header files / `.lib` to the `vkQuake` solution
* Build the `vkQuake` solution
* Copy `RayTracedGL1.dll` next to `vkQuake.exe` before running the executable
