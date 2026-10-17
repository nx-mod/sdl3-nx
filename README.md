# sdl3-nx

SDL 3 for Nintendo Switch homebrew. It has two parts:

- SDL 3.4.18 with devkitPro's Switch backend and the fixes around it;
- an SDL_GPU backend on [deko3d](https://github.com/devkitPro/deko3d), so SDL_GPU programs draw at
  the console's native speed without a Vulkan driver.

It is what [halo3-nx](https://github.com/nx-mod/halo3-nx) renders with.

The default branch is `switch-sdl3-deko3d`. The other branches mirror devkitPro's.

## What the Switch gets

- Video, audio, input (Joy-Con, Pro Controller, touch), filesystem and time.
- Application lifecycle (suspend, resume, focus), rumble, motion sensors and the software
  keyboard.
- **SDL_GPU on deko3d** (`SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_PRIVATE, ..., "deko3d")`):
  - shaders arrive as DKSH, which apps compile from GLSL with UAM;
  - each resource kind binds from slot 0.
  - Status: early. It runs a clear test on hardware, and halo3-nx's renderer starts on it.
- Optional Vulkan through Mesa's NVK with `-DSDL_VULKAN=ON`. This links the whole driver
  statically, which makes the result GPL.

## Building

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
  -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON \
  -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build build
cmake --install build --prefix <prefix>
```

Then call `find_package(SDL3 CONFIG)`, with `<prefix>` on `CMAKE_PREFIX_PATH`.

## Credits

- [SDL](https://github.com/libsdl-org/SDL): Sam Lantinga and the SDL contributors. zlib licence,
  see LICENSE.txt.
- [devkitPro](https://github.com/devkitPro/SDL): the Switch port, by Dave Murphy and contributors.
- p-sam and ROllerozxa: Switch locale, EGL, platform name and input fixes.
- [etonedemid](https://github.com/etonedemid/SDL): lifecycle, rumble, motion, the software
  keyboard, and the NVK Vulkan path.
- [deko3d](https://github.com/devkitPro/deko3d) and UAM: fincs.
- NVK on the Switch: [PalindromicBreadLoaf/nxvk](https://github.com/PalindromicBreadLoaf/nxvk)
  and [Mesa](https://mesa3d.org).
