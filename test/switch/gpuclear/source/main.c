/*
 * sdl3-nx gpuclear: the deko3d SDL_GPU backend's first test on hardware.
 * Creates the device (SDL_GPU_SHADERFORMAT_PRIVATE picks deko3d), claims the
 * window and clears the swapchain each frame to a colour that cycles. No
 * shaders. Logs to sdmc:/sdl3-nx-gpuclear.txt; + and - together quit.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>

static FILE *g_log;

static void say(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    if (g_log) {
        vfprintf(g_log, fmt, args);
        fputc('\n', g_log);
        fflush(g_log);
    }
    va_end(args);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    g_log = fopen("sdmc:/sdl3-nx-gpuclear.txt", "w");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        say("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("gpuclear", 1280, 720, 0);
    if (!window) {
        say("SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    SDL_GPUDevice *device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_PRIVATE, true, NULL);
    if (!device) {
        say("SDL_CreateGPUDevice: %s", SDL_GetError());
        return 1;
    }
    say("GPU driver: %s", SDL_GetGPUDeviceDriver(device));
    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        say("SDL_ClaimWindowForGPUDevice: %s", SDL_GetError());
        return 1;
    }
    say("window claimed; clearing");

    Uint64 frames = 0, start = SDL_GetTicks();
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }
        int count = 0;
        SDL_JoystickID *pads = SDL_GetGamepads(&count);
        for (int i = 0; pads && i < count; i++) {
            SDL_Gamepad *pad = SDL_GetGamepadFromID(pads[i]);
            if (!pad) {
                pad = SDL_OpenGamepad(pads[i]);
            }
            if (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START) && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK)) {
                running = false;
            }
        }
        SDL_free(pads);

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
        if (!cmd) {
            say("SDL_AcquireGPUCommandBuffer: %s", SDL_GetError());
            break;
        }
        SDL_GPUTexture *target = NULL;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &target, NULL, NULL)) {
            say("acquire swapchain: %s", SDL_GetError());
            break;
        }
        if (target) {
            float t = (float)(SDL_GetTicks() - start) / 1000.0f;
            SDL_GPUColorTargetInfo color = { 0 };
            color.texture = target;
            color.clear_color.r = 0.5f + 0.5f * SDL_sinf(t);
            color.clear_color.g = 0.5f + 0.5f * SDL_sinf(t + 2.1f);
            color.clear_color.b = 0.5f + 0.5f * SDL_sinf(t + 4.2f);
            color.clear_color.a = 1.0f;
            color.load_op = SDL_GPU_LOADOP_CLEAR;
            color.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, NULL);
            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
        if (++frames % 300 == 0) {
            say("%llu frames, %.1f fps", (unsigned long long)frames, frames * 1000.0 / (double)(SDL_GetTicks() - start));
        }
    }
    say("quitting after %llu frames", (unsigned long long)frames);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (g_log) {
        fclose(g_log);
    }
    return 0;
}
