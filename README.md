# sdl3-nx

SDL 3 for Nintendo Switch homebrew: SDL 3.4.18 with devkitPro's Switch backend
and the fixes around it, and an SDL_GPU backend on deko3d (in progress), so an
SDL_GPU program draws at the console's native speed with no Vulkan driver.

The default branch is `switch-sdl3-deko3d`. The other branches are devkitPro's
and are kept level with theirs.

## What the Switch gets

- Video, audio, input (Joy-Con, Pro Controller, touch), filesystem, time
- Application lifecycle (suspend, resume, focus), rumble, motion sensors,
  software keyboard
- SDL_GPU on deko3d (in progress)
- Optional Vulkan through Mesa's NVK, `-DSDL_VULKAN=ON` (links the whole
  driver statically; GPL)

## Building

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
  -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON \
  -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build build
cmake --install build --prefix <prefix>
```

Then `find_package(SDL3 CONFIG)` with `<prefix>` on `CMAKE_PREFIX_PATH`.

## Credits

- [SDL](https://github.com/libsdl-org/SDL): Sam Lantinga and the SDL
  contributors (zlib licence, see LICENSE.txt)
- [devkitPro](https://github.com/devkitPro/SDL): the Switch port (Dave Murphy
  and contributors)
- p-sam and ROllerozxa: Switch locale, EGL, platform name and input fixes
- [etonedemid](https://github.com/etonedemid/SDL): lifecycle, rumble, motion,
  software keyboard, and the NVK Vulkan path
- [deko3d](https://github.com/devkitPro/deko3d): fincs
- NVK on the Switch: [PalindromicBreadLoaf/nxvk](https://github.com/PalindromicBreadLoaf/nxvk)
  and [Mesa](https://mesa3d.org)
