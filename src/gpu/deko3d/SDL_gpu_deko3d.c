/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_GPU_DEKO3D

/*
 * SDL_GPU on deko3d, the Nintendo Switch's native graphics API (sdl3-nx).
 *
 * Shaders are DKSH binaries (deko3d's UAM output) passed as
 * SDL_GPU_SHADERFORMAT_PRIVATE. Bindings in the GLSL a DKSH is built from are
 * the slot numbers SDL_GPU uses, per resource type and stage:
 *   uniform buffers  layout(std140, binding = slot) uniform        (0-3)
 *   samplers         layout(binding = slot) uniform sampler*       (0-15)
 *   storage textures layout(binding = slot) uniform image*         (0-7)
 *   storage buffers  layout(std430, binding = slot) buffer         (0-7)
 * In a compute shader the read-write storage textures and buffers given to
 * SDL_BeginGPUComputePass follow the read-only ones: binding
 * num_readonly + i.
 *
 * deko3d has no pipeline objects: a pipeline here is the state structures,
 * bound when the pipeline is. Every pass starts with a full barrier, which
 * is what SDL_GPU's implicit synchronisation between passes needs.
 *
 * Parts of the memory handling follow libdol-nx's deko3d renderer
 * (nx-mod/libdol-nx, relicensed for this file): resources are freed only
 * once the GPU can no longer be using them, and command memory is added to
 * a command buffer a piece at a time as it fills.
 */

#include "../SDL_sysgpu.h"

#include <switch.h>
#include <deko3d.h>

// ---------------------------------------------------------------- limits

#define DKO_HEAP_BLOCK_SIZE (32u * 1024 * 1024)
#define DKO_CHUNK_SIZE (256u * 1024)
// Kept out of what each command chunk offers deko3d: 0.5.0's
// dkCmdBufBindVtxBuffers writes a word more per buffer than it reserves, and
// writes past a chunk's end land in the next chunk (nfsmw-nx's
// HowToDeko3d.md, item 17).
#define DKO_CHUNK_SLACK 256u
#define DKO_CODE_SIZE (32u * 1024 * 1024)
#define DKO_PER_WARP_SCRATCH (64u * 1024)
#define DKO_MAX_IMAGE_DESCRIPTORS 8192
#define DKO_MAX_SAMPLER_DESCRIPTORS 2048
#define DKO_MAX_SWAPCHAIN_IMAGES 3
#define DKO_MAX_VERTEX_ATTRIBUTES 32

typedef enum DKO_HeapKind
{
    DKO_HEAP_GPU,    // buffers and textures
    DKO_HEAP_UPLOAD, // transfer buffers, command and uniform memory: CPU writes, GPU reads
    DKO_HEAP_COUNT
} DKO_HeapKind;

// ---------------------------------------------------------------- types

typedef struct DKO_Range
{
    Uint32 offset;
    Uint32 size;
} DKO_Range;

typedef struct DKO_Block
{
    DkMemBlock memblock;
    Uint32 size;
    bool dedicated;
    DKO_Range *free;
    Uint32 freeCount;
    Uint32 freeCapacity;
    struct DKO_Block *next;
} DKO_Block;

typedef struct DKO_Heap
{
    SDL_Mutex *lock;
    Uint32 flags;
    DKO_Block *blocks;
} DKO_Heap;

typedef struct DKO_Alloc
{
    DKO_Block *block;
    Uint32 offset;
    Uint32 size;
    DKO_HeapKind kind;
} DKO_Alloc;

typedef struct DKO_Renderer DKO_Renderer;

typedef struct DKO_Buffer
{
    DKO_Alloc alloc;
    DkGpuAddr gpu;
    Uint32 size;
} DKO_Buffer;

typedef struct DKO_TransferBuffer
{
    DKO_Alloc alloc;
    Uint32 size;
    // Command buffers that will read or write the current backing. Cycling
    // gives a new backing and a new generation; a use of an older one,
    // finishing later, must not count against the new one.
    SDL_AtomicInt pendingUses;
    Uint32 generation;
} DKO_TransferBuffer;

typedef struct DKO_TransferUse
{
    DKO_TransferBuffer *transfer;
    Uint32 generation;
} DKO_TransferUse;

typedef struct DKO_Texture
{
    TextureCommonHeader common;
    DkImageLayout layout;
    DkImage image;
    DKO_Alloc alloc;
    DkImageFormat format;
    DkImageType type;
    Uint32 descriptor;        // image descriptor for sampling, or ~0
    Uint32 storageDescriptor; // image descriptor for load/store, or ~0
    bool swapchain;
} DKO_Texture;

typedef struct DKO_Sampler
{
    DkSampler sampler;
    Uint32 descriptor;
} DKO_Sampler;

typedef struct DKO_Shader
{
    // The caller's reference and one per pipeline: SDL lets a shader be
    // released as soon as its pipelines are made, and they still bind it.
    SDL_AtomicInt references;
    DkShader shader;
    Uint32 codeOffset;
    Uint32 codeSize;
    Uint32 numSamplers;
    Uint32 numStorageTextures;
    Uint32 numStorageBuffers;
    Uint32 numUniformBuffers;
} DKO_Shader;

typedef struct DKO_GraphicsPipeline
{
    GraphicsPipelineCommonHeader common;
    DKO_Shader *vertex;
    DKO_Shader *fragment;
    DkPrimitive primitive;
    DkRasterizerState rasterizer;
    DkColorState color;
    DkColorWriteState colorWrite;
    DkBlendState blend[MAX_COLOR_TARGET_BINDINGS];
    Uint32 numBlend;
    DkDepthStencilState depthStencil;
    Uint8 stencilCompareMask;
    Uint8 stencilWriteMask;
    DkMultisampleState multisample;
    Uint32 sampleMask;
    DkVtxAttribState attribs[DKO_MAX_VERTEX_ATTRIBUTES];
    Uint32 numAttribs;
    DkVtxBufferState buffers[MAX_VERTEX_BUFFERS];
    Uint32 numBuffers;
    float depthBiasConstant, depthBiasClamp, depthBiasSlope;
} DKO_GraphicsPipeline;

typedef struct DKO_ComputePipeline
{
    ComputePipelineCommonHeader common;
    DKO_Shader shader;
} DKO_ComputePipeline;

typedef struct DKO_Fence
{
    DkFence fence;
    SDL_AtomicInt references;
} DKO_Fence;

typedef struct DKO_WindowData
{
    SDL_Window *window;
    DkSwapchain swapchain;
    DKO_Texture images[DKO_MAX_SWAPCHAIN_IMAGES];
    Uint32 numImages;
    Uint32 width, height;
} DKO_WindowData;

typedef struct DKO_CommandBuffer
{
    CommandBufferCommonHeader common;
    DKO_Renderer *renderer;
    DkCmdBuf cmd;
    Uint64 id;
    DKO_Fence *fence;
    bool submitted;

    // memory taken from the chunk pool, given back once the GPU is done
    DKO_Alloc *chunks;
    Uint32 numChunks, chunkCapacity;
    DKO_Alloc uniformChunk;
    Uint32 uniformUsed;

    DKO_TransferUse *transfers;
    Uint32 numTransfers, transferCapacity;
    bool downloads;

    DKO_WindowData *presentWindow;
    int presentSlot;

    // bound state a draw needs
    DKO_GraphicsPipeline *graphics;
    DKO_ComputePipeline *compute;
    DkIdxFormat indexFormat;
    Uint32 indexCount; // indices the bound index buffer holds past its offset
    Uint8 stencilReference;
    Uint32 rwStorageTextureBase;
    Uint32 rwStorageBufferBase;
    DKO_Texture *resolveSources[MAX_COLOR_TARGET_BINDINGS];
    DKO_Texture *resolveTargets[MAX_COLOR_TARGET_BINDINGS];
    Uint32 resolveLevels[MAX_COLOR_TARGET_BINDINGS];
    Uint32 resolveLayers[MAX_COLOR_TARGET_BINDINGS];
    Uint32 numResolves;

    struct DKO_CommandBuffer *nextFree;
} DKO_CommandBuffer;

typedef enum DKO_ReleaseKind
{
    DKO_RELEASE_ALLOC,
    DKO_RELEASE_IMAGE_DESCRIPTOR,
    DKO_RELEASE_SAMPLER_DESCRIPTOR,
    DKO_RELEASE_CODE,
    DKO_RELEASE_MEMORY // SDL_free
} DKO_ReleaseKind;

typedef struct DKO_Release
{
    DKO_ReleaseKind kind;
    Uint64 afterId; // freed once every command buffer up to this id is done
    DKO_Alloc alloc;
    Uint32 value;
    Uint32 size;
    void *memory;
} DKO_Release;

struct DKO_Renderer
{
    SDL_GPUDevice *sdlDevice;
    DkDevice device;
    DkQueue queue;
    SDL_Mutex *submitLock;
    SDL_Mutex *stateLock; // command buffer bookkeeping, releases, descriptors, code

    DKO_Heap heaps[DKO_HEAP_COUNT];

    DkMemBlock descriptorBlock;
    DkGpuAddr imageDescriptors;
    DkGpuAddr samplerDescriptors;
    DkImageDescriptor *imageDescriptorCpu;
    DkSamplerDescriptor *samplerDescriptorCpu;
    Uint32 *freeImageDescriptors;
    Uint32 numFreeImageDescriptors;
    Uint32 *freeSamplerDescriptors;
    Uint32 numFreeSamplerDescriptors;

    DkMemBlock codeBlock;
    DKO_Range *codeFree;
    Uint32 codeFreeCount, codeFreeCapacity;

    DKO_Alloc *freeChunks;
    Uint32 numFreeChunks, freeChunkCapacity;

    DKO_CommandBuffer **active; // recording or in flight
    Uint32 numActive, activeCapacity;
    DKO_CommandBuffer *freeCommandBuffers;
    Uint64 nextId;

    DKO_Release *releases;
    Uint32 numReleases, releaseCapacity;

    DKO_WindowData *window; // one window on the Switch
    Uint32 allowedFramesInFlight;
};

// ---------------------------------------------------------------- logging

static void DKO_DebugCallback(void *userData, const char *context, DkResult result, const char *message)
{
    (void)userData;
    SDL_LogError(SDL_LOG_CATEGORY_GPU, "deko3d %s: %s (%d)", context, message ? message : "", (int)result);
}

static void DKO_Unsupported(const char *what)
{
    SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "deko3d: %s is not supported", what);
}

// ---------------------------------------------------------------- growable arrays

static bool DKO_Reserve(void **array, Uint32 *capacity, Uint32 count, size_t element)
{
    if (count < *capacity) {
        return true;
    }
    Uint32 grown = *capacity ? *capacity * 2 : 16;
    while (grown <= count) {
        grown *= 2;
    }
    void *bigger = SDL_realloc(*array, grown * element);
    if (!bigger) {
        return false;
    }
    *array = bigger;
    *capacity = grown;
    return true;
}

// ---------------------------------------------------------------- ranges (first fit, merged on free)

static bool DKO_RangeTake(DKO_Range **free, Uint32 *count, Uint32 size, Uint32 alignment, Uint32 *out)
{
    for (Uint32 i = 0; i < *count; i++) {
        DKO_Range *range = &(*free)[i];
        Uint32 start = (range->offset + alignment - 1) & ~(alignment - 1);
        Uint32 padding = start - range->offset;
        if (range->size < padding + size) {
            continue;
        }
        Uint32 end = start + size;
        Uint32 rangeEnd = range->offset + range->size;
        *out = start;
        if (padding == 0) {
            range->offset = end;
            range->size = rangeEnd - end;
            if (range->size == 0) {
                SDL_memmove(range, range + 1, (*count - i - 1) * sizeof(DKO_Range));
                (*count)--;
            }
        } else {
            range->size = padding;
            if (end < rangeEnd) {
                // the tail stays free after the padding (inserted in order)
                DKO_Range tail = { end, rangeEnd - end };
                SDL_memmove(range + 2, range + 1, (*count - i - 1) * sizeof(DKO_Range));
                range[1] = tail;
                (*count)++;
            }
        }
        return true;
    }
    return false;
}

static bool DKO_RangeGive(DKO_Range **free, Uint32 *count, Uint32 *capacity, Uint32 offset, Uint32 size)
{
    if (!DKO_Reserve((void **)free, capacity, *count + 1, sizeof(DKO_Range))) {
        return false;
    }
    Uint32 i = 0;
    while (i < *count && (*free)[i].offset < offset) {
        i++;
    }
    SDL_memmove(&(*free)[i + 1], &(*free)[i], (*count - i) * sizeof(DKO_Range));
    (*free)[i].offset = offset;
    (*free)[i].size = size;
    (*count)++;
    // merge with the neighbours
    if (i + 1 < *count && (*free)[i].offset + (*free)[i].size == (*free)[i + 1].offset) {
        (*free)[i].size += (*free)[i + 1].size;
        SDL_memmove(&(*free)[i + 1], &(*free)[i + 2], (*count - i - 2) * sizeof(DKO_Range));
        (*count)--;
    }
    if (i > 0 && (*free)[i - 1].offset + (*free)[i - 1].size == (*free)[i].offset) {
        (*free)[i - 1].size += (*free)[i].size;
        SDL_memmove(&(*free)[i], &(*free)[i + 1], (*count - i - 1) * sizeof(DKO_Range));
        (*count)--;
    }
    return true;
}

// ---------------------------------------------------------------- heaps

static DKO_Block *DKO_NewBlock(DKO_Renderer *renderer, DKO_Heap *heap, Uint32 size, bool dedicated)
{
    DkMemBlockMaker maker;
    size = (size + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
    dkMemBlockMakerDefaults(&maker, renderer->device, size);
    maker.flags = heap->flags;
    DkMemBlock memblock = dkMemBlockCreate(&maker);
    if (!memblock) {
        SDL_SetError("deko3d: can't allocate %u KiB of GPU memory", size / 1024);
        return NULL;
    }
    DKO_Block *block = SDL_calloc(1, sizeof(DKO_Block));
    if (!block) {
        dkMemBlockDestroy(memblock);
        return NULL;
    }
    block->memblock = memblock;
    block->size = size;
    block->dedicated = dedicated;
    if (!dedicated) {
        DKO_RangeGive(&block->free, &block->freeCount, &block->freeCapacity, 0, size);
    }
    block->next = heap->blocks;
    heap->blocks = block;
    return block;
}

static bool DKO_Allocate(DKO_Renderer *renderer, DKO_HeapKind kind, Uint32 size, Uint32 alignment, DKO_Alloc *out)
{
    DKO_Heap *heap = &renderer->heaps[kind];
    if (alignment < 16) {
        alignment = 16;
    }
    size = (size + 15) & ~15u;
    SDL_LockMutex(heap->lock);
    DKO_Block *chosen = NULL;
    Uint32 offset = 0;
    if (size > DKO_HEAP_BLOCK_SIZE / 2) {
        chosen = DKO_NewBlock(renderer, heap, size, true);
    } else {
        for (DKO_Block *block = heap->blocks; block; block = block->next) {
            if (!block->dedicated && DKO_RangeTake(&block->free, &block->freeCount, size, alignment, &offset)) {
                chosen = block;
                break;
            }
        }
        if (!chosen) {
            chosen = DKO_NewBlock(renderer, heap, DKO_HEAP_BLOCK_SIZE, false);
            if (chosen && !DKO_RangeTake(&chosen->free, &chosen->freeCount, size, alignment, &offset)) {
                chosen = NULL;
            }
        }
    }
    SDL_UnlockMutex(heap->lock);
    if (!chosen) {
        return false;
    }
    out->block = chosen;
    out->offset = offset;
    out->size = size;
    out->kind = kind;
    return true;
}

static void DKO_FreeNow(DKO_Renderer *renderer, DKO_Alloc *alloc)
{
    if (!alloc->block) {
        return;
    }
    DKO_Heap *heap = &renderer->heaps[alloc->kind];
    SDL_LockMutex(heap->lock);
    if (alloc->block->dedicated) {
        DKO_Block **link = &heap->blocks;
        while (*link && *link != alloc->block) {
            link = &(*link)->next;
        }
        if (*link) {
            *link = alloc->block->next;
        }
        dkMemBlockDestroy(alloc->block->memblock);
        SDL_free(alloc->block->free);
        SDL_free(alloc->block);
    } else {
        DKO_RangeGive(&alloc->block->free, &alloc->block->freeCount, &alloc->block->freeCapacity, alloc->offset, alloc->size);
    }
    SDL_UnlockMutex(heap->lock);
    alloc->block = NULL;
}

static DkGpuAddr DKO_Gpu(const DKO_Alloc *alloc)
{
    return dkMemBlockGetGpuAddr(alloc->block->memblock) + alloc->offset;
}

static Uint8 *DKO_Cpu(const DKO_Alloc *alloc)
{
    return (Uint8 *)dkMemBlockGetCpuAddr(alloc->block->memblock) + alloc->offset;
}

// ---------------------------------------------------------------- deferred release

// Called with stateLock held.
static void DKO_Defer(DKO_Renderer *renderer, const DKO_Release *release)
{
    if (!DKO_Reserve((void **)&renderer->releases, &renderer->releaseCapacity, renderer->numReleases + 1, sizeof(DKO_Release))) {
        return; // leaked rather than freed under the GPU
    }
    DKO_Release *slot = &renderer->releases[renderer->numReleases++];
    *slot = *release;
    slot->afterId = renderer->nextId; // every command buffer acquired so far
}

static void DKO_DeferAlloc(DKO_Renderer *renderer, const DKO_Alloc *alloc)
{
    if (!alloc->block) {
        return;
    }
    DKO_Release release = { DKO_RELEASE_ALLOC };
    release.alloc = *alloc;
    SDL_LockMutex(renderer->stateLock);
    DKO_Defer(renderer, &release);
    SDL_UnlockMutex(renderer->stateLock);
}

static void DKO_DeferValue(DKO_Renderer *renderer, DKO_ReleaseKind kind, Uint32 value, Uint32 size)
{
    DKO_Release release = { kind };
    release.value = value;
    release.size = size;
    SDL_LockMutex(renderer->stateLock);
    DKO_Defer(renderer, &release);
    SDL_UnlockMutex(renderer->stateLock);
}

static void DKO_DeferMemory(DKO_Renderer *renderer, void *memory)
{
    DKO_Release release = { DKO_RELEASE_MEMORY };
    release.memory = memory;
    SDL_LockMutex(renderer->stateLock);
    DKO_Defer(renderer, &release);
    SDL_UnlockMutex(renderer->stateLock);
}

// ---------------------------------------------------------------- descriptors and shader code (stateLock held)

static Uint32 DKO_TakeImageDescriptor(DKO_Renderer *renderer)
{
    return renderer->numFreeImageDescriptors ? renderer->freeImageDescriptors[--renderer->numFreeImageDescriptors] : ~0u;
}

static Uint32 DKO_TakeSamplerDescriptor(DKO_Renderer *renderer)
{
    return renderer->numFreeSamplerDescriptors ? renderer->freeSamplerDescriptors[--renderer->numFreeSamplerDescriptors] : ~0u;
}

static void DKO_ApplyRelease(DKO_Renderer *renderer, DKO_Release *release)
{
    switch (release->kind) {
    case DKO_RELEASE_ALLOC:
        DKO_FreeNow(renderer, &release->alloc);
        break;
    case DKO_RELEASE_IMAGE_DESCRIPTOR:
        renderer->freeImageDescriptors[renderer->numFreeImageDescriptors++] = release->value;
        break;
    case DKO_RELEASE_SAMPLER_DESCRIPTOR:
        renderer->freeSamplerDescriptors[renderer->numFreeSamplerDescriptors++] = release->value;
        break;
    case DKO_RELEASE_CODE:
        DKO_RangeGive(&renderer->codeFree, &renderer->codeFreeCount, &renderer->codeFreeCapacity, release->value, release->size);
        break;
    case DKO_RELEASE_MEMORY:
        SDL_free(release->memory);
        break;
    }
}

// ---------------------------------------------------------------- formats

static DkImageFormat DKO_ImageFormat(SDL_GPUTextureFormat format)
{
    switch (format) {
    case SDL_GPU_TEXTUREFORMAT_A8_UNORM: return DkImageFormat_R8_Unorm;
    case SDL_GPU_TEXTUREFORMAT_R8_UNORM: return DkImageFormat_R8_Unorm;
    case SDL_GPU_TEXTUREFORMAT_R8G8_UNORM: return DkImageFormat_RG8_Unorm;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM: return DkImageFormat_RGBA8_Unorm;
    case SDL_GPU_TEXTUREFORMAT_R16_UNORM: return DkImageFormat_R16_Unorm;
    case SDL_GPU_TEXTUREFORMAT_R16G16_UNORM: return DkImageFormat_RG16_Unorm;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM: return DkImageFormat_RGBA16_Unorm;
    case SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM: return DkImageFormat_RGB10A2_Unorm;
    case SDL_GPU_TEXTUREFORMAT_B5G6R5_UNORM: return DkImageFormat_RGB565_Unorm;
    case SDL_GPU_TEXTUREFORMAT_B5G5R5A1_UNORM: return DkImageFormat_BGR5A1_Unorm;
    case SDL_GPU_TEXTUREFORMAT_B4G4R4A4_UNORM: return DkImageFormat_RGBA4_Unorm;
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM: return DkImageFormat_BGRA8_Unorm;
    case SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM: return DkImageFormat_RGBA_BC1;
    case SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM: return DkImageFormat_RGBA_BC2;
    case SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM: return DkImageFormat_RGBA_BC3;
    case SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM: return DkImageFormat_R_BC4_Unorm;
    case SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM: return DkImageFormat_RG_BC5_Unorm;
    case SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM: return DkImageFormat_RGBA_BC7_Unorm;
    case SDL_GPU_TEXTUREFORMAT_BC6H_RGB_FLOAT: return DkImageFormat_RGBA_BC6H_SF16_Float;
    case SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT: return DkImageFormat_RGBA_BC6H_UF16_Float;
    case SDL_GPU_TEXTUREFORMAT_R8_SNORM: return DkImageFormat_R8_Snorm;
    case SDL_GPU_TEXTUREFORMAT_R8G8_SNORM: return DkImageFormat_RG8_Snorm;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_SNORM: return DkImageFormat_RGBA8_Snorm;
    case SDL_GPU_TEXTUREFORMAT_R16_SNORM: return DkImageFormat_R16_Snorm;
    case SDL_GPU_TEXTUREFORMAT_R16G16_SNORM: return DkImageFormat_RG16_Snorm;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_SNORM: return DkImageFormat_RGBA16_Snorm;
    case SDL_GPU_TEXTUREFORMAT_R16_FLOAT: return DkImageFormat_R16_Float;
    case SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT: return DkImageFormat_RG16_Float;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT: return DkImageFormat_RGBA16_Float;
    case SDL_GPU_TEXTUREFORMAT_R32_FLOAT: return DkImageFormat_R32_Float;
    case SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT: return DkImageFormat_RG32_Float;
    case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT: return DkImageFormat_RGBA32_Float;
    case SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT: return DkImageFormat_RG11B10_Float;
    case SDL_GPU_TEXTUREFORMAT_R8_UINT: return DkImageFormat_R8_Uint;
    case SDL_GPU_TEXTUREFORMAT_R8G8_UINT: return DkImageFormat_RG8_Uint;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UINT: return DkImageFormat_RGBA8_Uint;
    case SDL_GPU_TEXTUREFORMAT_R16_UINT: return DkImageFormat_R16_Uint;
    case SDL_GPU_TEXTUREFORMAT_R16G16_UINT: return DkImageFormat_RG16_Uint;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT: return DkImageFormat_RGBA16_Uint;
    case SDL_GPU_TEXTUREFORMAT_R32_UINT: return DkImageFormat_R32_Uint;
    case SDL_GPU_TEXTUREFORMAT_R32G32_UINT: return DkImageFormat_RG32_Uint;
    case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT: return DkImageFormat_RGBA32_Uint;
    case SDL_GPU_TEXTUREFORMAT_R8_INT: return DkImageFormat_R8_Sint;
    case SDL_GPU_TEXTUREFORMAT_R8G8_INT: return DkImageFormat_RG8_Sint;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_INT: return DkImageFormat_RGBA8_Sint;
    case SDL_GPU_TEXTUREFORMAT_R16_INT: return DkImageFormat_R16_Sint;
    case SDL_GPU_TEXTUREFORMAT_R16G16_INT: return DkImageFormat_RG16_Sint;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_INT: return DkImageFormat_RGBA16_Sint;
    case SDL_GPU_TEXTUREFORMAT_R32_INT: return DkImageFormat_R32_Sint;
    case SDL_GPU_TEXTUREFORMAT_R32G32_INT: return DkImageFormat_RG32_Sint;
    case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_INT: return DkImageFormat_RGBA32_Sint;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB: return DkImageFormat_RGBA8_Unorm_sRGB;
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB: return DkImageFormat_BGRA8_Unorm_sRGB;
    case SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM_SRGB: return DkImageFormat_RGBA_BC1_sRGB;
    case SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM_SRGB: return DkImageFormat_RGBA_BC2_sRGB;
    case SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM_SRGB: return DkImageFormat_RGBA_BC3_sRGB;
    case SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB: return DkImageFormat_RGBA_BC7_Unorm_sRGB;
    case SDL_GPU_TEXTUREFORMAT_D16_UNORM: return DkImageFormat_Z16;
    case SDL_GPU_TEXTUREFORMAT_D24_UNORM: return DkImageFormat_Z24X8;
    case SDL_GPU_TEXTUREFORMAT_D32_FLOAT: return DkImageFormat_ZF32;
    case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT: return DkImageFormat_Z24S8;
    case SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT: return DkImageFormat_ZF32_X24S8;
    case SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM: return DkImageFormat_RGBA_ASTC_4x4;
    case SDL_GPU_TEXTUREFORMAT_ASTC_5x4_UNORM: return DkImageFormat_RGBA_ASTC_5x4;
    case SDL_GPU_TEXTUREFORMAT_ASTC_5x5_UNORM: return DkImageFormat_RGBA_ASTC_5x5;
    case SDL_GPU_TEXTUREFORMAT_ASTC_6x5_UNORM: return DkImageFormat_RGBA_ASTC_6x5;
    case SDL_GPU_TEXTUREFORMAT_ASTC_6x6_UNORM: return DkImageFormat_RGBA_ASTC_6x6;
    case SDL_GPU_TEXTUREFORMAT_ASTC_8x5_UNORM: return DkImageFormat_RGBA_ASTC_8x5;
    case SDL_GPU_TEXTUREFORMAT_ASTC_8x6_UNORM: return DkImageFormat_RGBA_ASTC_8x6;
    case SDL_GPU_TEXTUREFORMAT_ASTC_8x8_UNORM: return DkImageFormat_RGBA_ASTC_8x8;
    case SDL_GPU_TEXTUREFORMAT_ASTC_10x5_UNORM: return DkImageFormat_RGBA_ASTC_10x5;
    case SDL_GPU_TEXTUREFORMAT_ASTC_10x6_UNORM: return DkImageFormat_RGBA_ASTC_10x6;
    case SDL_GPU_TEXTUREFORMAT_ASTC_10x8_UNORM: return DkImageFormat_RGBA_ASTC_10x8;
    case SDL_GPU_TEXTUREFORMAT_ASTC_10x10_UNORM: return DkImageFormat_RGBA_ASTC_10x10;
    case SDL_GPU_TEXTUREFORMAT_ASTC_12x10_UNORM: return DkImageFormat_RGBA_ASTC_12x10;
    case SDL_GPU_TEXTUREFORMAT_ASTC_12x12_UNORM: return DkImageFormat_RGBA_ASTC_12x12;
    default: return DkImageFormat_None;
    }
}

static bool DKO_IsDepthFormat(SDL_GPUTextureFormat format)
{
    return format == SDL_GPU_TEXTUREFORMAT_D16_UNORM || format == SDL_GPU_TEXTUREFORMAT_D24_UNORM ||
           format == SDL_GPU_TEXTUREFORMAT_D32_FLOAT || format == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT ||
           format == SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
}

static DkImageType DKO_ImageType(const SDL_GPUTextureCreateInfo *info)
{
    switch (info->type) {
    case SDL_GPU_TEXTURETYPE_2D_ARRAY: return info->sample_count > SDL_GPU_SAMPLECOUNT_1 ? DkImageType_2DMSArray : DkImageType_2DArray;
    case SDL_GPU_TEXTURETYPE_3D: return DkImageType_3D;
    case SDL_GPU_TEXTURETYPE_CUBE: return DkImageType_Cubemap;
    case SDL_GPU_TEXTURETYPE_CUBE_ARRAY: return DkImageType_CubemapArray;
    default: return info->sample_count > SDL_GPU_SAMPLECOUNT_1 ? DkImageType_2DMS : DkImageType_2D;
    }
}

static DkMsMode DKO_MsMode(SDL_GPUSampleCount count)
{
    switch (count) {
    case SDL_GPU_SAMPLECOUNT_2: return DkMsMode_2x;
    case SDL_GPU_SAMPLECOUNT_4: return DkMsMode_4x;
    case SDL_GPU_SAMPLECOUNT_8: return DkMsMode_8x;
    default: return DkMsMode_1x;
    }
}

static bool DKO_VertexFormat(SDL_GPUVertexElementFormat format, DkVtxAttribSize *size, DkVtxAttribType *type)
{
    switch (format) {
    case SDL_GPU_VERTEXELEMENTFORMAT_INT: *size = DkVtxAttribSize_1x32; *type = DkVtxAttribType_Sint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_INT2: *size = DkVtxAttribSize_2x32; *type = DkVtxAttribType_Sint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_INT3: *size = DkVtxAttribSize_3x32; *type = DkVtxAttribType_Sint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_INT4: *size = DkVtxAttribSize_4x32; *type = DkVtxAttribType_Sint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_UINT: *size = DkVtxAttribSize_1x32; *type = DkVtxAttribType_Uint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_UINT2: *size = DkVtxAttribSize_2x32; *type = DkVtxAttribType_Uint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_UINT3: *size = DkVtxAttribSize_3x32; *type = DkVtxAttribType_Uint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_UINT4: *size = DkVtxAttribSize_4x32; *type = DkVtxAttribType_Uint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT: *size = DkVtxAttribSize_1x32; *type = DkVtxAttribType_Float; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2: *size = DkVtxAttribSize_2x32; *type = DkVtxAttribType_Float; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3: *size = DkVtxAttribSize_3x32; *type = DkVtxAttribType_Float; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4: *size = DkVtxAttribSize_4x32; *type = DkVtxAttribType_Float; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_BYTE2: *size = DkVtxAttribSize_2x8; *type = DkVtxAttribType_Sint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_BYTE4: *size = DkVtxAttribSize_4x8; *type = DkVtxAttribType_Sint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2: *size = DkVtxAttribSize_2x8; *type = DkVtxAttribType_Uint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4: *size = DkVtxAttribSize_4x8; *type = DkVtxAttribType_Uint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_BYTE2_NORM: *size = DkVtxAttribSize_2x8; *type = DkVtxAttribType_Snorm; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM: *size = DkVtxAttribSize_4x8; *type = DkVtxAttribType_Snorm; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2_NORM: *size = DkVtxAttribSize_2x8; *type = DkVtxAttribType_Unorm; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM: *size = DkVtxAttribSize_4x8; *type = DkVtxAttribType_Unorm; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_SHORT2: *size = DkVtxAttribSize_2x16; *type = DkVtxAttribType_Sint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_SHORT4: *size = DkVtxAttribSize_4x16; *type = DkVtxAttribType_Sint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_USHORT2: *size = DkVtxAttribSize_2x16; *type = DkVtxAttribType_Uint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_USHORT4: *size = DkVtxAttribSize_4x16; *type = DkVtxAttribType_Uint; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_SHORT2_NORM: *size = DkVtxAttribSize_2x16; *type = DkVtxAttribType_Snorm; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM: *size = DkVtxAttribSize_4x16; *type = DkVtxAttribType_Snorm; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_USHORT2_NORM: *size = DkVtxAttribSize_2x16; *type = DkVtxAttribType_Unorm; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_USHORT4_NORM: *size = DkVtxAttribSize_4x16; *type = DkVtxAttribType_Unorm; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_HALF2: *size = DkVtxAttribSize_2x16; *type = DkVtxAttribType_Float; return true;
    case SDL_GPU_VERTEXELEMENTFORMAT_HALF4: *size = DkVtxAttribSize_4x16; *type = DkVtxAttribType_Float; return true;
    default: return false;
    }
}

static DkCompareOp DKO_Compare(SDL_GPUCompareOp op)
{
    switch (op) {
    case SDL_GPU_COMPAREOP_NEVER: return DkCompareOp_Never;
    case SDL_GPU_COMPAREOP_LESS: return DkCompareOp_Less;
    case SDL_GPU_COMPAREOP_EQUAL: return DkCompareOp_Equal;
    case SDL_GPU_COMPAREOP_LESS_OR_EQUAL: return DkCompareOp_Lequal;
    case SDL_GPU_COMPAREOP_GREATER: return DkCompareOp_Greater;
    case SDL_GPU_COMPAREOP_NOT_EQUAL: return DkCompareOp_NotEqual;
    case SDL_GPU_COMPAREOP_GREATER_OR_EQUAL: return DkCompareOp_Gequal;
    default: return DkCompareOp_Always;
    }
}

static DkStencilOp DKO_StencilOp(SDL_GPUStencilOp op)
{
    switch (op) {
    case SDL_GPU_STENCILOP_ZERO: return DkStencilOp_Zero;
    case SDL_GPU_STENCILOP_REPLACE: return DkStencilOp_Replace;
    case SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP: return DkStencilOp_Incr;
    case SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP: return DkStencilOp_Decr;
    case SDL_GPU_STENCILOP_INVERT: return DkStencilOp_Invert;
    case SDL_GPU_STENCILOP_INCREMENT_AND_WRAP: return DkStencilOp_IncrWrap;
    case SDL_GPU_STENCILOP_DECREMENT_AND_WRAP: return DkStencilOp_DecrWrap;
    default: return DkStencilOp_Keep;
    }
}

static DkBlendFactor DKO_BlendFactor(SDL_GPUBlendFactor factor)
{
    switch (factor) {
    case SDL_GPU_BLENDFACTOR_ZERO: return DkBlendFactor_Zero;
    case SDL_GPU_BLENDFACTOR_ONE: return DkBlendFactor_One;
    case SDL_GPU_BLENDFACTOR_SRC_COLOR: return DkBlendFactor_SrcColor;
    case SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR: return DkBlendFactor_InvSrcColor;
    case SDL_GPU_BLENDFACTOR_DST_COLOR: return DkBlendFactor_DstColor;
    case SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR: return DkBlendFactor_InvDstColor;
    case SDL_GPU_BLENDFACTOR_SRC_ALPHA: return DkBlendFactor_SrcAlpha;
    case SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA: return DkBlendFactor_InvSrcAlpha;
    case SDL_GPU_BLENDFACTOR_DST_ALPHA: return DkBlendFactor_DstAlpha;
    case SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA: return DkBlendFactor_InvDstAlpha;
    case SDL_GPU_BLENDFACTOR_CONSTANT_COLOR: return DkBlendFactor_ConstColor;
    case SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR: return DkBlendFactor_InvConstColor;
    case SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE: return DkBlendFactor_SrcAlphaSaturate;
    default: return DkBlendFactor_One;
    }
}

static DkBlendOp DKO_BlendOp(SDL_GPUBlendOp op)
{
    switch (op) {
    case SDL_GPU_BLENDOP_SUBTRACT: return DkBlendOp_Sub;
    case SDL_GPU_BLENDOP_REVERSE_SUBTRACT: return DkBlendOp_RevSub;
    case SDL_GPU_BLENDOP_MIN: return DkBlendOp_Min;
    case SDL_GPU_BLENDOP_MAX: return DkBlendOp_Max;
    default: return DkBlendOp_Add;
    }
}

static DkPrimitive DKO_Primitive(SDL_GPUPrimitiveType type)
{
    switch (type) {
    case SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP: return DkPrimitive_TriangleStrip;
    case SDL_GPU_PRIMITIVETYPE_LINELIST: return DkPrimitive_Lines;
    case SDL_GPU_PRIMITIVETYPE_LINESTRIP: return DkPrimitive_LineStrip;
    case SDL_GPU_PRIMITIVETYPE_POINTLIST: return DkPrimitive_Points;
    default: return DkPrimitive_Triangles;
    }
}

static DkWrapMode DKO_Wrap(SDL_GPUSamplerAddressMode mode)
{
    switch (mode) {
    case SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT: return DkWrapMode_MirroredRepeat;
    case SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE: return DkWrapMode_ClampToEdge;
    default: return DkWrapMode_Repeat;
    }
}

// ---------------------------------------------------------------- resources

static SDL_GPUBuffer *DEKO3D_CreateBuffer(SDL_GPURenderer *driverData, SDL_GPUBufferUsageFlags usageFlags, Uint32 size, const char *debugName)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    (void)usageFlags;
    (void)debugName;
    DKO_Buffer *buffer = SDL_calloc(1, sizeof(DKO_Buffer));
    if (!buffer) {
        return NULL;
    }
    if (!DKO_Allocate(renderer, DKO_HEAP_GPU, size ? size : 16, 256, &buffer->alloc)) {
        SDL_free(buffer);
        return NULL;
    }
    buffer->gpu = DKO_Gpu(&buffer->alloc);
    buffer->size = size;
    return (SDL_GPUBuffer *)buffer;
}

static void DEKO3D_ReleaseBuffer(SDL_GPURenderer *driverData, SDL_GPUBuffer *handle)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    DKO_Buffer *buffer = (DKO_Buffer *)handle;
    DKO_DeferAlloc(renderer, &buffer->alloc);
    DKO_DeferMemory(renderer, buffer);
}

static SDL_GPUTransferBuffer *DEKO3D_CreateTransferBuffer(SDL_GPURenderer *driverData, SDL_GPUTransferBufferUsage usage, Uint32 size, const char *debugName)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    (void)usage;
    (void)debugName;
    DKO_TransferBuffer *transfer = SDL_calloc(1, sizeof(DKO_TransferBuffer));
    if (!transfer) {
        return NULL;
    }
    if (!DKO_Allocate(renderer, DKO_HEAP_UPLOAD, size ? size : 16, 256, &transfer->alloc)) {
        SDL_free(transfer);
        return NULL;
    }
    transfer->size = size;
    return (SDL_GPUTransferBuffer *)transfer;
}

static void DEKO3D_ReleaseTransferBuffer(SDL_GPURenderer *driverData, SDL_GPUTransferBuffer *handle)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    DKO_TransferBuffer *transfer = (DKO_TransferBuffer *)handle;
    DKO_DeferAlloc(renderer, &transfer->alloc);
    DKO_DeferMemory(renderer, transfer);
}

static void *DEKO3D_MapTransferBuffer(SDL_GPURenderer *driverData, SDL_GPUTransferBuffer *handle, bool cycle)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    DKO_TransferBuffer *transfer = (DKO_TransferBuffer *)handle;
    if (cycle && SDL_GetAtomicInt(&transfer->pendingUses) > 0) {
        // a fresh backing, so what the GPU will still read is left alone
        DKO_Alloc fresh;
        if (DKO_Allocate(renderer, DKO_HEAP_UPLOAD, transfer->size ? transfer->size : 16, 256, &fresh)) {
            DKO_DeferAlloc(renderer, &transfer->alloc);
            SDL_LockMutex(renderer->stateLock); // against DKO_Recycle's check of the generation
            transfer->alloc = fresh;
            transfer->generation++;
            SDL_SetAtomicInt(&transfer->pendingUses, 0);
            SDL_UnlockMutex(renderer->stateLock);
        }
    }
    return DKO_Cpu(&transfer->alloc);
}

static void DEKO3D_UnmapTransferBuffer(SDL_GPURenderer *driverData, SDL_GPUTransferBuffer *handle)
{
    (void)driverData;
    (void)handle; // uncached memory: nothing to flush
}

// deko3d 0.5.0's tile height bug: when level 0 has 6 to 8 rows of 8-row
// blocks, its layout and its descriptor disagree about the tile height, so
// level 0's second column of blocks lands on mip 1. A custom, shrunk tile
// size makes them agree (WWHD's image_tile_size_fix, via nfsmw-nx's
// HowToDeko3d.md, item 3).
static void DKO_FixTileSize(DkImageLayoutMaker *maker, Uint32 height)
{
    if (maker->flags & (DkImageFlags_PitchLinear | DkImageFlags_CustomTileSize | DkImageFlags_UsageVideo)) {
        return;
    }
    if (maker->type == DkImageType_1D || maker->type == DkImageType_1DArray || maker->type == DkImageType_3D || maker->type == DkImageType_Buffer) {
        return;
    }
    bool blocks4 = maker->format >= DkImageFormat_RGB_BC1 && maker->format <= DkImageFormat_RGBA_BC6H_UF16_Float;
    Uint32 rows = blocks4 ? (height + 3) / 4 : height; // level 0's height in blocks
    if ((maker->type == DkImageType_2DMS || maker->type == DkImageType_2DMSArray) && (maker->msMode == DkMsMode_4x || maker->msMode == DkMsMode_8x)) {
        rows *= 2;
    }
    Uint32 gobs = (rows + rows / 2 + 7) / 8; // deko3d's pickTileSize
    Uint32 tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
    Uint32 shrunk = tile; // its level-0 shrink (calcLevelOffset)
    while (shrunk && (8u << (shrunk - 1)) >= rows) {
        shrunk--;
    }
    if (shrunk == tile) {
        return;
    }
    maker->flags |= DkImageFlags_CustomTileSize;
    maker->tileSize = (DkTileSize)shrunk;
}

static Uint32 DKO_WriteImageDescriptor(DKO_Renderer *renderer, DKO_Texture *texture, bool loadStore)
{
    SDL_LockMutex(renderer->stateLock);
    Uint32 slot = DKO_TakeImageDescriptor(renderer);
    SDL_UnlockMutex(renderer->stateLock);
    if (slot == ~0u) {
        SDL_SetError("deko3d: out of image descriptors");
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "deko3d: out of image descriptors (%u); the texture won't sample", DKO_MAX_IMAGE_DESCRIPTORS);
        return ~0u;
    }
    DkImageView view;
    dkImageViewDefaults(&view, &texture->image);
    view.type = texture->type;
    if (DKO_IsDepthFormat(texture->common.info.format)) {
        view.dsSource = DkDsSource_Depth;
    }
    if (texture->common.info.format == SDL_GPU_TEXTUREFORMAT_A8_UNORM) {
        view.swizzle[0] = DkImageSwizzle_Zero;
        view.swizzle[1] = DkImageSwizzle_Zero;
        view.swizzle[2] = DkImageSwizzle_Zero;
        view.swizzle[3] = DkImageSwizzle_Red;
    }
    dkImageDescriptorInitialize(&renderer->imageDescriptorCpu[slot], &view, loadStore, false);
    return slot;
}

static SDL_GPUTexture *DEKO3D_CreateTexture(SDL_GPURenderer *driverData, const SDL_GPUTextureCreateInfo *info)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    DkImageFormat format = DKO_ImageFormat(info->format);
    if (format == DkImageFormat_None) {
        SDL_SetError("deko3d: texture format %d is not supported", (int)info->format);
        return NULL;
    }
    DKO_Texture *texture = SDL_calloc(1, sizeof(DKO_Texture));
    if (!texture) {
        return NULL;
    }
    texture->common.info = *info;
    texture->format = format;
    texture->type = DKO_ImageType(info);
    texture->descriptor = ~0u;
    texture->storageDescriptor = ~0u;

    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, renderer->device);
    maker.type = texture->type;
    maker.format = format;
    maker.msMode = DKO_MsMode(info->sample_count);
    maker.mipLevels = info->num_levels ? info->num_levels : 1;
    maker.dimensions[0] = info->width;
    maker.dimensions[1] = info->height;
    maker.dimensions[2] = info->layer_count_or_depth ? info->layer_count_or_depth : 1;
    if (texture->type == DkImageType_Cubemap) {
        maker.dimensions[2] = 6;
    }
    // No DkImageFlags_HwCompression: the copy engine (downloads, texture
    // copies) reads a compressed surface's raw bytes, and nouveau saw the
    // same with depth (nfsmw-nx's HowToDeko3d.md, 5.10). Worth measuring
    // again for color targets that are never copied.
    if (info->usage & (SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
        maker.flags |= DkImageFlags_UsageRender;
    }
    // Blits (SDL_BlitGPUTexture, mipmap generation) run on the 2D engine,
    // which needs the image laid out for it: without the flag deko3d's
    // debug build refuses the blit and the release build faults the GPU.
    // Every format but the 96-bit ones can, and no 3D image.
    if ((info->usage & (SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET)) && texture->type != DkImageType_3D &&
        format != DkImageFormat_RGB32_Float && format != DkImageFormat_RGB32_Uint && format != DkImageFormat_RGB32_Sint) {
        maker.flags |= DkImageFlags_Usage2DEngine;
    }
    if (info->usage & (SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
                       SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE)) {
        maker.flags |= DkImageFlags_UsageLoadStore;
        maker.flags &= ~DkImageFlags_HwCompression;
    }
    DKO_FixTileSize(&maker, info->height);
    dkImageLayoutInitialize(&texture->layout, &maker);

    Uint32 size = (Uint32)dkImageLayoutGetSize(&texture->layout);
    Uint32 alignment = dkImageLayoutGetAlignment(&texture->layout);
    if (!DKO_Allocate(renderer, DKO_HEAP_GPU, size, alignment, &texture->alloc)) {
        SDL_free(texture);
        return NULL;
    }
    dkImageInitialize(&texture->image, &texture->layout, texture->alloc.block->memblock, texture->alloc.offset);

    if (info->usage & SDL_GPU_TEXTUREUSAGE_SAMPLER) {
        texture->descriptor = DKO_WriteImageDescriptor(renderer, texture, false);
    }
    if (maker.flags & DkImageFlags_UsageLoadStore) {
        texture->storageDescriptor = DKO_WriteImageDescriptor(renderer, texture, true);
    }
    return (SDL_GPUTexture *)texture;
}

static void DEKO3D_ReleaseTexture(SDL_GPURenderer *driverData, SDL_GPUTexture *handle)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    DKO_Texture *texture = (DKO_Texture *)handle;
    if (texture->swapchain) {
        return;
    }
    if (texture->descriptor != ~0u) {
        DKO_DeferValue(renderer, DKO_RELEASE_IMAGE_DESCRIPTOR, texture->descriptor, 0);
    }
    if (texture->storageDescriptor != ~0u) {
        DKO_DeferValue(renderer, DKO_RELEASE_IMAGE_DESCRIPTOR, texture->storageDescriptor, 0);
    }
    DKO_DeferAlloc(renderer, &texture->alloc);
    DKO_DeferMemory(renderer, texture);
}

static SDL_GPUSampler *DEKO3D_CreateSampler(SDL_GPURenderer *driverData, const SDL_GPUSamplerCreateInfo *info)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    DKO_Sampler *sampler = SDL_calloc(1, sizeof(DKO_Sampler));
    if (!sampler) {
        return NULL;
    }
    dkSamplerDefaults(&sampler->sampler);
    sampler->sampler.minFilter = info->min_filter == SDL_GPU_FILTER_LINEAR ? DkFilter_Linear : DkFilter_Nearest;
    sampler->sampler.magFilter = info->mag_filter == SDL_GPU_FILTER_LINEAR ? DkFilter_Linear : DkFilter_Nearest;
    sampler->sampler.mipFilter = info->mipmap_mode == SDL_GPU_SAMPLERMIPMAPMODE_LINEAR ? DkMipFilter_Linear : DkMipFilter_Nearest;
    sampler->sampler.wrapMode[0] = DKO_Wrap(info->address_mode_u);
    sampler->sampler.wrapMode[1] = DKO_Wrap(info->address_mode_v);
    sampler->sampler.wrapMode[2] = DKO_Wrap(info->address_mode_w);
    sampler->sampler.lodBias = info->mip_lod_bias;
    sampler->sampler.lodClampMin = info->min_lod;
    sampler->sampler.lodClampMax = info->max_lod;
    sampler->sampler.compareEnable = info->enable_compare;
    sampler->sampler.compareOp = DKO_Compare(info->compare_op);
    sampler->sampler.maxAnisotropy = info->enable_anisotropy ? SDL_clamp(info->max_anisotropy, 1.0f, 16.0f) : 1.0f;

    SDL_LockMutex(renderer->stateLock);
    sampler->descriptor = DKO_TakeSamplerDescriptor(renderer);
    SDL_UnlockMutex(renderer->stateLock);
    if (sampler->descriptor == ~0u) {
        SDL_free(sampler);
        SDL_SetError("deko3d: out of sampler descriptors");
        return NULL;
    }
    dkSamplerDescriptorInitialize(&renderer->samplerDescriptorCpu[sampler->descriptor], &sampler->sampler);
    return (SDL_GPUSampler *)sampler;
}

static void DEKO3D_ReleaseSampler(SDL_GPURenderer *driverData, SDL_GPUSampler *handle)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    DKO_Sampler *sampler = (DKO_Sampler *)handle;
    DKO_DeferValue(renderer, DKO_RELEASE_SAMPLER_DESCRIPTOR, sampler->descriptor, 0);
    DKO_DeferMemory(renderer, sampler);
}

// A DKSH binary: its header, the control section, then the code.
typedef struct DKO_DkshHeader
{
    Uint32 magic, headerSize, controlSize, codeSize, programsOffset, programCount;
} DKO_DkshHeader;

static bool DKO_LoadShader(DKO_Renderer *renderer, const Uint8 *code, size_t size, DKO_Shader *out)
{
    DKO_DkshHeader header;
    if (size < sizeof(header)) {
        return SDL_SetError("deko3d: shader is not a DKSH");
    }
    SDL_memcpy(&header, code, sizeof(header));
    if (header.magic != 0x48534b44 || (size_t)header.controlSize + header.codeSize > size) {
        return SDL_SetError("deko3d: shader is not a DKSH");
    }
    // Each program header: type, entry point, GPRs, constbuf1 offset and
    // size, then the scratch memory it needs per warp.
    for (Uint32 i = 0; i < header.programCount; i++) {
        Uint32 scratch;
        size_t at = (size_t)header.programsOffset + i * 64 + 5 * sizeof(Uint32);
        if (at + sizeof(scratch) <= header.controlSize) {
            SDL_memcpy(&scratch, code + at, sizeof(scratch));
            if (scratch > DKO_PER_WARP_SCRATCH) {
                SDL_LogError(SDL_LOG_CATEGORY_GPU, "deko3d: a shader needs %u bytes of scratch per warp, the queue has %u; its draws will trap",
                             scratch, DKO_PER_WARP_SCRATCH);
            }
        }
    }
    Uint32 offset;
    SDL_LockMutex(renderer->stateLock);
    bool placed = DKO_RangeTake(&renderer->codeFree, &renderer->codeFreeCount, header.codeSize, DK_SHADER_CODE_ALIGNMENT, &offset);
    SDL_UnlockMutex(renderer->stateLock);
    if (!placed) {
        return SDL_SetError("deko3d: shader code memory is full (%u MiB)", DKO_CODE_SIZE >> 20);
    }
    SDL_memcpy((Uint8 *)dkMemBlockGetCpuAddr(renderer->codeBlock) + offset, code + header.controlSize, header.codeSize);
    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, renderer->codeBlock, offset);
    maker.control = code;
    dkShaderInitialize(&out->shader, &maker);
    if (!dkShaderIsValid(&out->shader)) {
        DKO_DeferValue(renderer, DKO_RELEASE_CODE, offset, header.codeSize);
        return SDL_SetError("deko3d: the DKSH did not load");
    }
    out->codeOffset = offset;
    out->codeSize = header.codeSize;
    SDL_SetAtomicInt(&out->references, 1);
    return true;
}

static SDL_GPUShader *DEKO3D_CreateShader(SDL_GPURenderer *driverData, const SDL_GPUShaderCreateInfo *info)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    if (info->format != SDL_GPU_SHADERFORMAT_PRIVATE) {
        SDL_SetError("deko3d: shaders are DKSH (SDL_GPU_SHADERFORMAT_PRIVATE)");
        return NULL;
    }
    DKO_Shader *shader = SDL_calloc(1, sizeof(DKO_Shader));
    if (!shader) {
        return NULL;
    }
    if (!DKO_LoadShader(renderer, info->code, info->code_size, shader)) {
        SDL_free(shader);
        return NULL;
    }
    shader->numSamplers = info->num_samplers;
    shader->numStorageTextures = info->num_storage_textures;
    shader->numStorageBuffers = info->num_storage_buffers;
    shader->numUniformBuffers = info->num_uniform_buffers;
    return (SDL_GPUShader *)shader;
}

// Drops a reference; the last one gives the code back (once the GPU is done).
static void DKO_UnrefShader(DKO_Renderer *renderer, DKO_Shader *shader)
{
    if (shader && SDL_AddAtomicInt(&shader->references, -1) == 1) {
        DKO_DeferValue(renderer, DKO_RELEASE_CODE, shader->codeOffset, shader->codeSize);
        DKO_DeferMemory(renderer, shader);
    }
}

static void DEKO3D_ReleaseShader(SDL_GPURenderer *driverData, SDL_GPUShader *handle)
{
    DKO_UnrefShader((DKO_Renderer *)driverData, (DKO_Shader *)handle);
}

static SDL_GPUComputePipeline *DEKO3D_CreateComputePipeline(SDL_GPURenderer *driverData, const SDL_GPUComputePipelineCreateInfo *info)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    if (info->format != SDL_GPU_SHADERFORMAT_PRIVATE) {
        SDL_SetError("deko3d: shaders are DKSH (SDL_GPU_SHADERFORMAT_PRIVATE)");
        return NULL;
    }
    DKO_ComputePipeline *pipeline = SDL_calloc(1, sizeof(DKO_ComputePipeline));
    if (!pipeline) {
        return NULL;
    }
    if (!DKO_LoadShader(renderer, info->code, info->code_size, &pipeline->shader)) {
        SDL_free(pipeline);
        return NULL;
    }
    pipeline->common.numSamplers = info->num_samplers;
    pipeline->common.numReadonlyStorageTextures = info->num_readonly_storage_textures;
    pipeline->common.numReadonlyStorageBuffers = info->num_readonly_storage_buffers;
    pipeline->common.numReadWriteStorageTextures = info->num_readwrite_storage_textures;
    pipeline->common.numReadWriteStorageBuffers = info->num_readwrite_storage_buffers;
    pipeline->common.numUniformBuffers = info->num_uniform_buffers;
    return (SDL_GPUComputePipeline *)pipeline;
}

static void DEKO3D_ReleaseComputePipeline(SDL_GPURenderer *driverData, SDL_GPUComputePipeline *handle)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    DKO_ComputePipeline *pipeline = (DKO_ComputePipeline *)handle;
    DKO_DeferValue(renderer, DKO_RELEASE_CODE, pipeline->shader.codeOffset, pipeline->shader.codeSize);
    DKO_DeferMemory(renderer, pipeline);
}

static SDL_GPUGraphicsPipeline *DEKO3D_CreateGraphicsPipeline(SDL_GPURenderer *driverData, const SDL_GPUGraphicsPipelineCreateInfo *info)
{
    (void)driverData;
    DKO_GraphicsPipeline *pipeline = SDL_calloc(1, sizeof(DKO_GraphicsPipeline));
    if (!pipeline) {
        return NULL;
    }
    pipeline->vertex = (DKO_Shader *)info->vertex_shader;
    pipeline->fragment = (DKO_Shader *)info->fragment_shader;
    SDL_AddAtomicInt(&pipeline->vertex->references, 1);
    SDL_AddAtomicInt(&pipeline->fragment->references, 1);
    pipeline->primitive = DKO_Primitive(info->primitive_type);

    pipeline->common.num_vertex_samplers = pipeline->vertex->numSamplers;
    pipeline->common.num_vertex_storage_textures = pipeline->vertex->numStorageTextures;
    pipeline->common.num_vertex_storage_buffers = pipeline->vertex->numStorageBuffers;
    pipeline->common.num_vertex_uniform_buffers = pipeline->vertex->numUniformBuffers;
    pipeline->common.num_fragment_samplers = pipeline->fragment->numSamplers;
    pipeline->common.num_fragment_storage_textures = pipeline->fragment->numStorageTextures;
    pipeline->common.num_fragment_storage_buffers = pipeline->fragment->numStorageBuffers;
    pipeline->common.num_fragment_uniform_buffers = pipeline->fragment->numUniformBuffers;

    // vertex input: attributes by location, buffers by slot
    const SDL_GPUVertexInputState *input = &info->vertex_input_state;
    for (Uint32 i = 0; i < DKO_MAX_VERTEX_ATTRIBUTES; i++) {
        pipeline->attribs[i].isFixed = 1; // reads zero unless an attribute is given
        pipeline->attribs[i].size = DkVtxAttribSize_1x32;
        pipeline->attribs[i].type = DkVtxAttribType_Float;
    }
    for (Uint32 i = 0; i < input->num_vertex_attributes; i++) {
        const SDL_GPUVertexAttribute *attribute = &input->vertex_attributes[i];
        if (attribute->location >= DKO_MAX_VERTEX_ATTRIBUTES) {
            continue;
        }
        DkVtxAttribState *state = &pipeline->attribs[attribute->location];
        DkVtxAttribSize size;
        DkVtxAttribType type;
        if (!DKO_VertexFormat(attribute->format, &size, &type)) {
            continue;
        }
        state->isFixed = 0;
        state->bufferId = attribute->buffer_slot;
        state->offset = attribute->offset;
        state->size = size;
        state->type = type;
        if (attribute->location + 1 > pipeline->numAttribs) {
            pipeline->numAttribs = attribute->location + 1;
        }
    }
    for (Uint32 i = 0; i < input->num_vertex_buffers; i++) {
        const SDL_GPUVertexBufferDescription *description = &input->vertex_buffer_descriptions[i];
        if (description->slot >= MAX_VERTEX_BUFFERS) {
            continue;
        }
        pipeline->buffers[description->slot].stride = description->pitch;
        pipeline->buffers[description->slot].divisor =
            description->input_rate == SDL_GPU_VERTEXINPUTRATE_INSTANCE ? (description->instance_step_rate ? description->instance_step_rate : 1) : 0;
        if (description->slot + 1 > pipeline->numBuffers) {
            pipeline->numBuffers = description->slot + 1;
        }
    }

    // rasterizer
    const SDL_GPURasterizerState *raster = &info->rasterizer_state;
    dkRasterizerStateDefaults(&pipeline->rasterizer);
    pipeline->rasterizer.polygonModeFront = pipeline->rasterizer.polygonModeBack =
        raster->fill_mode == SDL_GPU_FILLMODE_LINE ? DkPolygonMode_Line : DkPolygonMode_Fill;
    pipeline->rasterizer.cullMode = raster->cull_mode == SDL_GPU_CULLMODE_FRONT ? DkFace_Front :
                                    raster->cull_mode == SDL_GPU_CULLMODE_BACK  ? DkFace_Back :
                                                                                   DkFace_None;
    pipeline->rasterizer.frontFace = raster->front_face == SDL_GPU_FRONTFACE_CLOCKWISE ? DkFrontFace_CW : DkFrontFace_CCW;
    pipeline->rasterizer.depthClampEnable = !raster->enable_depth_clip;
    if (raster->enable_depth_bias) {
        pipeline->rasterizer.depthBiasEnableMask = 7; // points, lines, polygons
        pipeline->depthBiasConstant = raster->depth_bias_constant_factor;
        pipeline->depthBiasClamp = raster->depth_bias_clamp;
        pipeline->depthBiasSlope = raster->depth_bias_slope_factor;
    }

    // color targets
    dkColorStateDefaults(&pipeline->color);
    dkColorWriteStateDefaults(&pipeline->colorWrite);
    pipeline->colorWrite.masks = 0;
    pipeline->numBlend = info->target_info.num_color_targets;
    for (Uint32 i = 0; i < info->target_info.num_color_targets && i < MAX_COLOR_TARGET_BINDINGS; i++) {
        const SDL_GPUColorTargetBlendState *blend = &info->target_info.color_target_descriptions[i].blend_state;
        dkBlendStateDefaults(&pipeline->blend[i]);
        pipeline->blend[i].colorBlendOp = DKO_BlendOp(blend->color_blend_op);
        pipeline->blend[i].srcColorBlendFactor = DKO_BlendFactor(blend->src_color_blendfactor);
        pipeline->blend[i].dstColorBlendFactor = DKO_BlendFactor(blend->dst_color_blendfactor);
        pipeline->blend[i].alphaBlendOp = DKO_BlendOp(blend->alpha_blend_op);
        pipeline->blend[i].srcAlphaBlendFactor = DKO_BlendFactor(blend->src_alpha_blendfactor);
        pipeline->blend[i].dstAlphaBlendFactor = DKO_BlendFactor(blend->dst_alpha_blendfactor);
        if (blend->enable_blend) {
            pipeline->color.blendEnableMask |= 1u << i;
        }
        Uint32 mask = blend->enable_color_write_mask ? (Uint32)blend->color_write_mask : 0xF;
        pipeline->colorWrite.masks |= (mask & 0xF) << (i * 4);
    }

    // depth and stencil
    const SDL_GPUDepthStencilState *ds = &info->depth_stencil_state;
    dkDepthStencilStateDefaults(&pipeline->depthStencil);
    pipeline->depthStencil.depthTestEnable = ds->enable_depth_test;
    pipeline->depthStencil.depthWriteEnable = ds->enable_depth_write;
    pipeline->depthStencil.depthCompareOp = DKO_Compare(ds->compare_op);
    pipeline->depthStencil.stencilTestEnable = ds->enable_stencil_test;
    pipeline->depthStencil.stencilFrontFailOp = DKO_StencilOp(ds->front_stencil_state.fail_op);
    pipeline->depthStencil.stencilFrontPassOp = DKO_StencilOp(ds->front_stencil_state.pass_op);
    pipeline->depthStencil.stencilFrontDepthFailOp = DKO_StencilOp(ds->front_stencil_state.depth_fail_op);
    pipeline->depthStencil.stencilFrontCompareOp = DKO_Compare(ds->front_stencil_state.compare_op);
    pipeline->depthStencil.stencilBackFailOp = DKO_StencilOp(ds->back_stencil_state.fail_op);
    pipeline->depthStencil.stencilBackPassOp = DKO_StencilOp(ds->back_stencil_state.pass_op);
    pipeline->depthStencil.stencilBackDepthFailOp = DKO_StencilOp(ds->back_stencil_state.depth_fail_op);
    pipeline->depthStencil.stencilBackCompareOp = DKO_Compare(ds->back_stencil_state.compare_op);
    pipeline->stencilCompareMask = ds->compare_mask;
    pipeline->stencilWriteMask = ds->write_mask;

    // multisampling
    dkMultisampleStateDefaults(&pipeline->multisample);
    pipeline->multisample.mode = DKO_MsMode(info->multisample_state.sample_count);
    pipeline->multisample.rasterizerMode = pipeline->multisample.mode;
    pipeline->multisample.alphaToCoverageEnable = info->multisample_state.enable_alpha_to_coverage;
    // The register has a bit per sample, 16 at most; a set bit above those
    // is a method error (GR exception) when the pipeline is bound.
    pipeline->sampleMask = (info->multisample_state.enable_mask ? info->multisample_state.sample_mask : 0xFFFFFFFF) & 0xFFFF;
    return (SDL_GPUGraphicsPipeline *)pipeline;
}

static void DEKO3D_ReleaseGraphicsPipeline(SDL_GPURenderer *driverData, SDL_GPUGraphicsPipeline *handle)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    DKO_GraphicsPipeline *pipeline = (DKO_GraphicsPipeline *)handle;
    DKO_UnrefShader(renderer, pipeline->vertex);
    DKO_UnrefShader(renderer, pipeline->fragment);
    DKO_DeferMemory(renderer, handle);
}

static void DEKO3D_SetBufferName(SDL_GPURenderer *driverData, SDL_GPUBuffer *buffer, const char *text)
{
    (void)driverData;
    (void)buffer;
    (void)text;
}

static void DEKO3D_SetTextureName(SDL_GPURenderer *driverData, SDL_GPUTexture *texture, const char *text)
{
    (void)driverData;
    (void)texture;
    (void)text;
}

static void DEKO3D_InsertDebugLabel(SDL_GPUCommandBuffer *commandBuffer, const char *text)
{
    (void)commandBuffer;
    (void)text;
}

static void DEKO3D_PushDebugGroup(SDL_GPUCommandBuffer *commandBuffer, const char *name)
{
    (void)commandBuffer;
    (void)name;
}

static void DEKO3D_PopDebugGroup(SDL_GPUCommandBuffer *commandBuffer)
{
    (void)commandBuffer;
}

// ==================================================================== command buffers (part 2 follows)

// Every command buffer acquired so far that the GPU has not finished with
// holds back the releases after it. Called with stateLock held.
static void DKO_Collect(DKO_Renderer *renderer);

static bool DKO_TakeChunk(DKO_Renderer *renderer, DKO_Alloc *out)
{
    SDL_LockMutex(renderer->stateLock);
    bool reused = renderer->numFreeChunks > 0;
    if (reused) {
        *out = renderer->freeChunks[--renderer->numFreeChunks];
    }
    SDL_UnlockMutex(renderer->stateLock);
    return reused || DKO_Allocate(renderer, DKO_HEAP_UPLOAD, DKO_CHUNK_SIZE, DK_MEMBLOCK_ALIGNMENT, out);
}

static void DKO_KeepChunk(DKO_CommandBuffer *cb, const DKO_Alloc *chunk)
{
    if (DKO_Reserve((void **)&cb->chunks, &cb->chunkCapacity, cb->numChunks + 1, sizeof(DKO_Alloc))) {
        cb->chunks[cb->numChunks++] = *chunk;
    }
}

// deko3d asks for more command memory as a buffer fills
static void DKO_CommandMemoryNeeded(void *userData, DkCmdBuf cmd, size_t minimum)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)userData;
    DKO_Alloc chunk;
    if (minimum > DKO_CHUNK_SIZE || !DKO_TakeChunk(cb->renderer, &chunk)) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "deko3d: no command memory");
        return;
    }
    DKO_KeepChunk(cb, &chunk);
    dkCmdBufAddMemory(cmd, chunk.block->memblock, chunk.offset, chunk.size - DKO_CHUNK_SLACK);
}

static DkGpuAddr DKO_UniformSpace(DKO_CommandBuffer *cb, const void *data, Uint32 length, Uint32 *size)
{
    Uint32 aligned = (length + DK_UNIFORM_BUF_ALIGNMENT - 1) & ~(DK_UNIFORM_BUF_ALIGNMENT - 1);
    if (!cb->uniformChunk.block || cb->uniformUsed + aligned > cb->uniformChunk.size) {
        DKO_Alloc chunk;
        if (aligned > DKO_CHUNK_SIZE || !DKO_TakeChunk(cb->renderer, &chunk)) {
            return 0;
        }
        DKO_KeepChunk(cb, &chunk);
        cb->uniformChunk = chunk;
        cb->uniformUsed = 0;
    }
    DkGpuAddr gpu = DKO_Gpu(&cb->uniformChunk) + cb->uniformUsed;
    SDL_memcpy(DKO_Cpu(&cb->uniformChunk) + cb->uniformUsed, data, length);
    cb->uniformUsed += aligned;
    *size = aligned;
    return gpu;
}

static void DKO_UseTransfer(DKO_CommandBuffer *cb, DKO_TransferBuffer *transfer)
{
    if (DKO_Reserve((void **)&cb->transfers, &cb->transferCapacity, cb->numTransfers + 1, sizeof(DKO_TransferUse))) {
        SDL_LockMutex(cb->renderer->stateLock);
        cb->transfers[cb->numTransfers].transfer = transfer;
        cb->transfers[cb->numTransfers].generation = transfer->generation;
        cb->numTransfers++;
        SDL_AddAtomicInt(&transfer->pendingUses, 1);
        SDL_UnlockMutex(cb->renderer->stateLock);
    }
}

static void DKO_ReleaseFenceReference(DKO_Fence *fence)
{
    if (fence && SDL_AddAtomicInt(&fence->references, -1) == 1) {
        SDL_free(fence);
    }
}

// The GPU is done with `cb`: its memory goes back, the object is reused.
static void DKO_Recycle(DKO_Renderer *renderer, DKO_CommandBuffer *cb)
{
    for (Uint32 i = 0; i < cb->numChunks; i++) {
        if (DKO_Reserve((void **)&renderer->freeChunks, &renderer->freeChunkCapacity, renderer->numFreeChunks + 1, sizeof(DKO_Alloc))) {
            renderer->freeChunks[renderer->numFreeChunks++] = cb->chunks[i];
        }
    }
    cb->numChunks = 0;
    cb->uniformChunk.block = NULL;
    for (Uint32 i = 0; i < cb->numTransfers; i++) {
        DKO_TransferUse *use = &cb->transfers[i];
        if (use->generation == use->transfer->generation) { // an older backing's use counts for nothing now
            SDL_AddAtomicInt(&use->transfer->pendingUses, -1);
        }
    }
    cb->numTransfers = 0;
    DKO_ReleaseFenceReference(cb->fence);
    cb->fence = NULL;
    cb->nextFree = renderer->freeCommandBuffers;
    renderer->freeCommandBuffers = cb;
}

static void DKO_Collect(DKO_Renderer *renderer)
{
    Uint64 oldest = renderer->nextId + 1;
    for (Uint32 i = 0; i < renderer->numActive;) {
        DKO_CommandBuffer *cb = renderer->active[i];
        if (cb->submitted && dkFenceWait(&cb->fence->fence, 0) == DkResult_Success) {
            renderer->active[i] = renderer->active[--renderer->numActive];
            DKO_Recycle(renderer, cb);
            continue;
        }
        if (cb->id < oldest) {
            oldest = cb->id;
        }
        i++;
    }
    for (Uint32 i = 0; i < renderer->numReleases;) {
        if (renderer->releases[i].afterId < oldest) {
            DKO_ApplyRelease(renderer, &renderer->releases[i]);
            renderer->releases[i] = renderer->releases[--renderer->numReleases];
            continue;
        }
        i++;
    }
}

static SDL_GPUCommandBuffer *DEKO3D_AcquireCommandBuffer(SDL_GPURenderer *driverData)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    SDL_LockMutex(renderer->stateLock);
    DKO_Collect(renderer);
    DKO_CommandBuffer *cb = renderer->freeCommandBuffers;
    if (cb) {
        renderer->freeCommandBuffers = cb->nextFree;
    }
    SDL_UnlockMutex(renderer->stateLock);

    if (!cb) {
        cb = SDL_calloc(1, sizeof(DKO_CommandBuffer));
        if (!cb) {
            return NULL;
        }
        cb->renderer = renderer;
        DkCmdBufMaker maker;
        dkCmdBufMakerDefaults(&maker, renderer->device);
        maker.userData = cb;
        maker.cbAddMem = DKO_CommandMemoryNeeded;
        cb->cmd = dkCmdBufCreate(&maker);
        if (!cb->cmd) {
            SDL_free(cb);
            return NULL;
        }
    }
    cb->fence = SDL_calloc(1, sizeof(DKO_Fence));
    if (!cb->fence) {
        return NULL;
    }
    SDL_SetAtomicInt(&cb->fence->references, 1);
    cb->submitted = false;
    cb->downloads = false;
    cb->presentWindow = NULL;
    cb->presentSlot = -1;
    cb->graphics = NULL;
    cb->compute = NULL;
    cb->indexFormat = DkIdxFormat_Uint16;
    cb->indexCount = 0;
    cb->stencilReference = 0;
    cb->numResolves = 0;
    SDL_zero(cb->common);

    // A cleared DkCmdBuf goes on writing at the start of the last chunk it
    // was given, and DKO_Recycle has handed that chunk back to the pool, so
    // another command buffer may be writing there too (the GPU then fetches
    // garbage: a PBDMA error, "GPU rejected command list"). Give it memory
    // of its own before anything is recorded.
    DKO_Alloc chunk;
    if (!DKO_TakeChunk(renderer, &chunk)) {
        SDL_SetError("deko3d: no command memory");
        return NULL;
    }
    DKO_KeepChunk(cb, &chunk);
    dkCmdBufAddMemory(cb->cmd, chunk.block->memblock, chunk.offset, chunk.size - DKO_CHUNK_SLACK);

    // every command buffer starts with the descriptor sets bound
    dkCmdBufBindImageDescriptorSet(cb->cmd, renderer->imageDescriptors, DKO_MAX_IMAGE_DESCRIPTORS);
    dkCmdBufBindSamplerDescriptorSet(cb->cmd, renderer->samplerDescriptors, DKO_MAX_SAMPLER_DESCRIPTORS);

    SDL_LockMutex(renderer->stateLock);
    cb->id = ++renderer->nextId;
    if (DKO_Reserve((void **)&renderer->active, &renderer->activeCapacity, renderer->numActive + 1, sizeof(DKO_CommandBuffer *))) {
        renderer->active[renderer->numActive++] = cb;
    }
    SDL_UnlockMutex(renderer->stateLock);
    return (SDL_GPUCommandBuffer *)cb;
}

// ---------------------------------------------------------------- passes

static void DKO_PassBarrier(DKO_CommandBuffer *cb, bool copy)
{
    // SDL_GPU orders passes for the caller: a full barrier between them, and
    // around copies the caches CPU-written memory may be stale in
    dkCmdBufBarrier(cb->cmd, DkBarrier_Full,
                    DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors |
                        (copy ? DkInvalidateFlags_L2Cache : 0));
}

static void DKO_View(DkImageView *view, DKO_Texture *texture, Uint32 level, Uint32 layer, bool renderTarget)
{
    dkImageViewDefaults(view, &texture->image);
    view->mipLevelOffset = (uint8_t)level;
    view->mipLevelCount = 1;
    view->layerOffset = (uint16_t)layer;
    view->layerCount = 1;
    if (renderTarget && texture->type == DkImageType_3D) {
        view->type = DkImageType_2DArray;
    } else if (renderTarget && (texture->type == DkImageType_Cubemap || texture->type == DkImageType_CubemapArray)) {
        view->type = DkImageType_2DArray;
    }
}

static void DEKO3D_BeginRenderPass(SDL_GPUCommandBuffer *commandBuffer, const SDL_GPUColorTargetInfo *colorTargetInfos, Uint32 numColorTargets,
                                   const SDL_GPUDepthStencilTargetInfo *depthStencilTargetInfo)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_PassBarrier(cb, false);

    DkImageView colorViews[MAX_COLOR_TARGET_BINDINGS];
    DkImageView const *colorPointers[MAX_COLOR_TARGET_BINDINGS];
    DkImageView depthView;
    Uint32 width = 0, height = 0;
    cb->numResolves = 0;
    for (Uint32 i = 0; i < numColorTargets; i++) {
        DKO_Texture *texture = (DKO_Texture *)colorTargetInfos[i].texture;
        DKO_View(&colorViews[i], texture, colorTargetInfos[i].mip_level, colorTargetInfos[i].layer_or_depth_plane, true);
        colorPointers[i] = &colorViews[i];
        Uint32 w = SDL_max(texture->common.info.width >> colorTargetInfos[i].mip_level, 1u);
        Uint32 h = SDL_max(texture->common.info.height >> colorTargetInfos[i].mip_level, 1u);
        width = i == 0 ? w : SDL_min(width, w);
        height = i == 0 ? h : SDL_min(height, h);
        if ((colorTargetInfos[i].store_op == SDL_GPU_STOREOP_RESOLVE || colorTargetInfos[i].store_op == SDL_GPU_STOREOP_RESOLVE_AND_STORE) &&
            colorTargetInfos[i].resolve_texture) {
            Uint32 r = cb->numResolves++;
            cb->resolveSources[r] = texture;
            cb->resolveTargets[r] = (DKO_Texture *)colorTargetInfos[i].resolve_texture;
            cb->resolveLevels[r] = colorTargetInfos[i].resolve_mip_level;
            cb->resolveLayers[r] = colorTargetInfos[i].resolve_layer;
        }
    }
    DkImageView const *depthPointer = NULL;
    if (depthStencilTargetInfo) {
        DKO_Texture *texture = (DKO_Texture *)depthStencilTargetInfo->texture;
        DKO_View(&depthView, texture, 0, 0, true);
        depthPointer = &depthView;
        if (numColorTargets == 0) {
            width = texture->common.info.width;
            height = texture->common.info.height;
        }
    }
    dkCmdBufBindRenderTargets(cb->cmd, colorPointers, numColorTargets, depthPointer);

    // Clears respect the scissor, which may still be the last pass's
    DkScissor whole = { 0, 0, width, height };
    dkCmdBufSetScissors(cb->cmd, 0, &whole, 1);

    for (Uint32 i = 0; i < numColorTargets; i++) {
        if (colorTargetInfos[i].load_op == SDL_GPU_LOADOP_CLEAR) {
            const SDL_FColor *c = &colorTargetInfos[i].clear_color;
            dkCmdBufClearColorFloat(cb->cmd, i, DkColorMask_RGBA, c->r, c->g, c->b, c->a);
        }
        // LOADOP_DONT_CARE keeps what's there: dkCmdBufDiscardColor really
        // throws it away, where desktop drivers in practice keep it, and
        // callers come to rely on that.
    }
    if (depthStencilTargetInfo) {
        bool clearDepth = depthStencilTargetInfo->load_op == SDL_GPU_LOADOP_CLEAR;
        bool clearStencil = depthStencilTargetInfo->stencil_load_op == SDL_GPU_LOADOP_CLEAR;
        if (clearDepth || clearStencil) {
            dkCmdBufClearDepthStencil(cb->cmd, clearDepth, depthStencilTargetInfo->clear_depth, clearStencil ? 0xFF : 0,
                                      depthStencilTargetInfo->clear_stencil);
        }
    }

    // SDL_GPU starts a pass with the viewport and scissor covering the target
    DkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    DkScissor scissor = { 0, 0, width, height };
    dkCmdBufSetViewports(cb->cmd, 0, &viewport, 1);
    dkCmdBufSetScissors(cb->cmd, 0, &scissor, 1);
}

static void DEKO3D_EndRenderPass(SDL_GPUCommandBuffer *commandBuffer)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    if (cb->numResolves) {
        dkCmdBufBarrier(cb->cmd, DkBarrier_Fragments, 0);
        for (Uint32 i = 0; i < cb->numResolves; i++) {
            DkImageView source, target;
            DKO_View(&source, cb->resolveSources[i], 0, 0, false);
            DKO_View(&target, cb->resolveTargets[i], cb->resolveLevels[i], cb->resolveLayers[i], false);
            dkCmdBufResolveImage(cb->cmd, &source, &target);
        }
        cb->numResolves = 0;
    }
    cb->graphics = NULL;
}

static void DEKO3D_BindGraphicsPipeline(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUGraphicsPipeline *handle)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_GraphicsPipeline *pipeline = (DKO_GraphicsPipeline *)handle;
    cb->graphics = pipeline;
    DkShader const *shaders[2] = { &pipeline->vertex->shader, &pipeline->fragment->shader };
    dkCmdBufBindShaders(cb->cmd, DkStageFlag_GraphicsMask, shaders, 2);
    dkCmdBufBindRasterizerState(cb->cmd, &pipeline->rasterizer);
    dkCmdBufBindColorState(cb->cmd, &pipeline->color);
    dkCmdBufBindColorWriteState(cb->cmd, &pipeline->colorWrite);
    if (pipeline->numBlend) {
        dkCmdBufBindBlendStates(cb->cmd, 0, pipeline->blend, pipeline->numBlend);
    }
    dkCmdBufBindDepthStencilState(cb->cmd, &pipeline->depthStencil);
    dkCmdBufBindMultisampleState(cb->cmd, &pipeline->multisample);
    dkCmdBufSetSampleMask(cb->cmd, pipeline->sampleMask);
    dkCmdBufSetStencil(cb->cmd, DkFace_FrontAndBack, pipeline->stencilWriteMask, cb->stencilReference, pipeline->stencilCompareMask);
    if (pipeline->rasterizer.depthBiasEnableMask) {
        dkCmdBufSetDepthBias(cb->cmd, pipeline->depthBiasConstant, pipeline->depthBiasClamp, pipeline->depthBiasSlope);
    }
    dkCmdBufBindVtxAttribState(cb->cmd, pipeline->attribs, pipeline->numAttribs);
    dkCmdBufBindVtxBufferState(cb->cmd, pipeline->buffers, pipeline->numBuffers);
}

static void DEKO3D_SetViewport(SDL_GPUCommandBuffer *commandBuffer, const SDL_GPUViewport *viewport)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DkViewport v = { viewport->x, viewport->y, viewport->w, viewport->h, viewport->min_depth, viewport->max_depth };
    dkCmdBufSetViewports(cb->cmd, 0, &v, 1);
}

static void DEKO3D_SetScissor(SDL_GPUCommandBuffer *commandBuffer, const SDL_Rect *scissor)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DkScissor s = { (uint32_t)SDL_max(scissor->x, 0), (uint32_t)SDL_max(scissor->y, 0), (uint32_t)SDL_max(scissor->w, 0), (uint32_t)SDL_max(scissor->h, 0) };
    dkCmdBufSetScissors(cb->cmd, 0, &s, 1);
}

static void DEKO3D_SetBlendConstants(SDL_GPUCommandBuffer *commandBuffer, SDL_FColor blendConstants)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    dkCmdBufSetBlendConst(cb->cmd, blendConstants.r, blendConstants.g, blendConstants.b, blendConstants.a);
}

static void DEKO3D_SetStencilReference(SDL_GPUCommandBuffer *commandBuffer, Uint8 reference)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    cb->stencilReference = reference;
    Uint8 writeMask = cb->graphics ? cb->graphics->stencilWriteMask : 0xFF;
    Uint8 compareMask = cb->graphics ? cb->graphics->stencilCompareMask : 0xFF;
    dkCmdBufSetStencil(cb->cmd, DkFace_FrontAndBack, writeMask, reference, compareMask);
}

static void DEKO3D_BindVertexBuffers(SDL_GPUCommandBuffer *commandBuffer, Uint32 firstSlot, const SDL_GPUBufferBinding *bindings, Uint32 numBindings)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    for (Uint32 i = 0; i < numBindings; i++) {
        DKO_Buffer *buffer = (DKO_Buffer *)bindings[i].buffer;
        dkCmdBufBindVtxBuffer(cb->cmd, firstSlot + i, buffer->gpu + bindings[i].offset, buffer->size - bindings[i].offset);
    }
}

static void DEKO3D_BindIndexBuffer(SDL_GPUCommandBuffer *commandBuffer, const SDL_GPUBufferBinding *binding, SDL_GPUIndexElementSize indexElementSize)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_Buffer *buffer = (DKO_Buffer *)binding->buffer;
    cb->indexFormat = indexElementSize == SDL_GPU_INDEXELEMENTSIZE_32BIT ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16;
    dkCmdBufBindIdxBuffer(cb->cmd, cb->indexFormat, buffer->gpu + binding->offset);
    Uint32 elementSize = indexElementSize == SDL_GPU_INDEXELEMENTSIZE_32BIT ? 4 : 2;
    cb->indexCount = binding->offset < buffer->size ? (buffer->size - binding->offset) / elementSize : 0;
}

static void DKO_BindSamplers(DKO_CommandBuffer *cb, DkStage stage, Uint32 firstSlot, const SDL_GPUTextureSamplerBinding *bindings, Uint32 numBindings)
{
    DkResHandle handles[MAX_TEXTURE_SAMPLERS_PER_STAGE];
    numBindings = SDL_min(numBindings, MAX_TEXTURE_SAMPLERS_PER_STAGE);
    for (Uint32 i = 0; i < numBindings; i++) {
        DKO_Texture *texture = (DKO_Texture *)bindings[i].texture;
        DKO_Sampler *sampler = (DKO_Sampler *)bindings[i].sampler;
        handles[i] = dkMakeTextureHandle(texture->descriptor, sampler->descriptor);
    }
    dkCmdBufBindTextures(cb->cmd, stage, firstSlot, handles, numBindings);
}

static void DKO_BindStorageTextures(DKO_CommandBuffer *cb, DkStage stage, Uint32 firstSlot, SDL_GPUTexture *const *textures, Uint32 numBindings)
{
    DkResHandle handles[MAX_STORAGE_TEXTURES_PER_STAGE + MAX_COMPUTE_WRITE_TEXTURES];
    numBindings = SDL_min(numBindings, (Uint32)SDL_arraysize(handles));
    for (Uint32 i = 0; i < numBindings; i++) {
        handles[i] = dkMakeImageHandle(((DKO_Texture *)textures[i])->storageDescriptor);
    }
    dkCmdBufBindImages(cb->cmd, stage, firstSlot, handles, numBindings);
}

static void DKO_BindStorageBuffers(DKO_CommandBuffer *cb, DkStage stage, Uint32 firstSlot, SDL_GPUBuffer *const *buffers, Uint32 numBindings)
{
    DkBufExtents extents[MAX_STORAGE_BUFFERS_PER_STAGE + MAX_COMPUTE_WRITE_BUFFERS];
    numBindings = SDL_min(numBindings, (Uint32)SDL_arraysize(extents));
    for (Uint32 i = 0; i < numBindings; i++) {
        DKO_Buffer *buffer = (DKO_Buffer *)buffers[i];
        extents[i].addr = buffer->gpu;
        extents[i].size = buffer->size;
    }
    dkCmdBufBindStorageBuffers(cb->cmd, stage, firstSlot, extents, numBindings);
}

static void DKO_PushUniforms(DKO_CommandBuffer *cb, DkStage stage, Uint32 slot, const void *data, Uint32 length)
{
    Uint32 size;
    DkGpuAddr address = DKO_UniformSpace(cb, data, length, &size);
    if (address) {
        dkCmdBufBindUniformBuffer(cb->cmd, stage, slot, address, size);
    }
}

static void DEKO3D_BindVertexSamplers(SDL_GPUCommandBuffer *commandBuffer, Uint32 firstSlot, const SDL_GPUTextureSamplerBinding *bindings, Uint32 numBindings)
{
    DKO_BindSamplers((DKO_CommandBuffer *)commandBuffer, DkStage_Vertex, firstSlot, bindings, numBindings);
}

static void DEKO3D_BindFragmentSamplers(SDL_GPUCommandBuffer *commandBuffer, Uint32 firstSlot, const SDL_GPUTextureSamplerBinding *bindings, Uint32 numBindings)
{
    DKO_BindSamplers((DKO_CommandBuffer *)commandBuffer, DkStage_Fragment, firstSlot, bindings, numBindings);
}

static void DEKO3D_BindVertexStorageTextures(SDL_GPUCommandBuffer *commandBuffer, Uint32 firstSlot, SDL_GPUTexture *const *textures, Uint32 numBindings)
{
    DKO_BindStorageTextures((DKO_CommandBuffer *)commandBuffer, DkStage_Vertex, firstSlot, textures, numBindings);
}

static void DEKO3D_BindFragmentStorageTextures(SDL_GPUCommandBuffer *commandBuffer, Uint32 firstSlot, SDL_GPUTexture *const *textures, Uint32 numBindings)
{
    DKO_BindStorageTextures((DKO_CommandBuffer *)commandBuffer, DkStage_Fragment, firstSlot, textures, numBindings);
}

static void DEKO3D_BindVertexStorageBuffers(SDL_GPUCommandBuffer *commandBuffer, Uint32 firstSlot, SDL_GPUBuffer *const *buffers, Uint32 numBindings)
{
    DKO_BindStorageBuffers((DKO_CommandBuffer *)commandBuffer, DkStage_Vertex, firstSlot, buffers, numBindings);
}

static void DEKO3D_BindFragmentStorageBuffers(SDL_GPUCommandBuffer *commandBuffer, Uint32 firstSlot, SDL_GPUBuffer *const *buffers, Uint32 numBindings)
{
    DKO_BindStorageBuffers((DKO_CommandBuffer *)commandBuffer, DkStage_Fragment, firstSlot, buffers, numBindings);
}

static void DEKO3D_PushVertexUniformData(SDL_GPUCommandBuffer *commandBuffer, Uint32 slotIndex, const void *data, Uint32 length)
{
    DKO_PushUniforms((DKO_CommandBuffer *)commandBuffer, DkStage_Vertex, slotIndex, data, length);
}

static void DEKO3D_PushFragmentUniformData(SDL_GPUCommandBuffer *commandBuffer, Uint32 slotIndex, const void *data, Uint32 length)
{
    DKO_PushUniforms((DKO_CommandBuffer *)commandBuffer, DkStage_Fragment, slotIndex, data, length);
}

static void DKO_IndexOverrun(Uint32 firstIndex, Uint32 numIndices, Uint32 bound)
{
    static SDL_AtomicInt reported;
    if (SDL_AtomicIncRef(&reported) < 8) {
        SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "deko3d: indexed draw of %u indices from %u reads past its index buffer (%u); clamped", numIndices,
                    firstIndex, bound);
    }
}

static void DEKO3D_DrawPrimitives(SDL_GPUCommandBuffer *commandBuffer, Uint32 numVertices, Uint32 numInstances, Uint32 firstVertex, Uint32 firstInstance)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    if (cb->graphics) {
        dkCmdBufDraw(cb->cmd, cb->graphics->primitive, numVertices, numInstances, firstVertex, firstInstance);
    }
}

static void DEKO3D_DrawIndexedPrimitives(SDL_GPUCommandBuffer *commandBuffer, Uint32 numIndices, Uint32 numInstances, Uint32 firstIndex, Sint32 vertexOffset,
                                         Uint32 firstInstance)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    // deko3d 0.5.0 sets no index limit (the register is left at the top of
    // the address space), so indices past the bound buffer are fetched from
    // whatever follows: an unmapped page faults the GPU mid-draw. Vulkan
    // keeps them inside the buffer; so does this.
    if (firstIndex >= cb->indexCount) {
        DKO_IndexOverrun(firstIndex, numIndices, cb->indexCount);
        return;
    }
    if (numIndices > cb->indexCount - firstIndex) {
        DKO_IndexOverrun(firstIndex, numIndices, cb->indexCount);
        numIndices = cb->indexCount - firstIndex;
    }
    if (cb->graphics) {
        dkCmdBufDrawIndexed(cb->cmd, cb->graphics->primitive, numIndices, numInstances, firstIndex, vertexOffset, firstInstance);
    }
}

static void DEKO3D_DrawPrimitivesIndirect(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUBuffer *handle, Uint32 offset, Uint32 drawCount)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_Buffer *buffer = (DKO_Buffer *)handle;
    for (Uint32 i = 0; cb->graphics && i < drawCount; i++) {
        dkCmdBufDrawIndirect(cb->cmd, cb->graphics->primitive, buffer->gpu + offset + i * sizeof(SDL_GPUIndirectDrawCommand));
    }
}

static void DEKO3D_DrawIndexedPrimitivesIndirect(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUBuffer *handle, Uint32 offset, Uint32 drawCount)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_Buffer *buffer = (DKO_Buffer *)handle;
    for (Uint32 i = 0; cb->graphics && i < drawCount; i++) {
        dkCmdBufDrawIndexedIndirect(cb->cmd, cb->graphics->primitive, buffer->gpu + offset + i * sizeof(SDL_GPUIndexedIndirectDrawCommand));
    }
}

// ---------------------------------------------------------------- compute

static void DEKO3D_BeginComputePass(SDL_GPUCommandBuffer *commandBuffer, const SDL_GPUStorageTextureReadWriteBinding *storageTextureBindings,
                                    Uint32 numStorageTextureBindings, const SDL_GPUStorageBufferReadWriteBinding *storageBufferBindings,
                                    Uint32 numStorageBufferBindings)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_PassBarrier(cb, false);
    cb->compute = NULL;
    // bound after the read-only ones once the pipeline says how many those are;
    // kept until then
    cb->rwStorageTextureBase = 0;
    cb->rwStorageBufferBase = 0;
    SDL_GPUTexture *textures[MAX_COMPUTE_WRITE_TEXTURES];
    SDL_GPUBuffer *buffers[MAX_COMPUTE_WRITE_BUFFERS];
    Uint32 nt = SDL_min(numStorageTextureBindings, (Uint32)MAX_COMPUTE_WRITE_TEXTURES);
    Uint32 nb = SDL_min(numStorageBufferBindings, (Uint32)MAX_COMPUTE_WRITE_BUFFERS);
    for (Uint32 i = 0; i < nt; i++) {
        textures[i] = storageTextureBindings[i].texture;
    }
    for (Uint32 i = 0; i < nb; i++) {
        buffers[i] = storageBufferBindings[i].buffer;
    }
    // Stashed in the common header's pass record by SDL; rebound at pipeline bind.
    SDL_memcpy(cb->resolveSources, textures, sizeof(SDL_GPUTexture *) * SDL_min(nt, (Uint32)MAX_COLOR_TARGET_BINDINGS));
    cb->numResolves = 0;
    if (nt) {
        DKO_BindStorageTextures(cb, DkStage_Compute, 0, textures, nt);
    }
    if (nb) {
        DKO_BindStorageBuffers(cb, DkStage_Compute, 0, buffers, nb);
    }
    cb->rwStorageTextureBase = nt;
    cb->rwStorageBufferBase = nb;
}

static void DEKO3D_BindComputePipeline(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUComputePipeline *handle)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_ComputePipeline *pipeline = (DKO_ComputePipeline *)handle;
    cb->compute = pipeline;
    DkShader const *shaders[1] = { &pipeline->shader.shader };
    dkCmdBufBindShaders(cb->cmd, DkStageFlag_Compute, shaders, 1);
}

static void DEKO3D_BindComputeSamplers(SDL_GPUCommandBuffer *commandBuffer, Uint32 firstSlot, const SDL_GPUTextureSamplerBinding *bindings, Uint32 numBindings)
{
    DKO_BindSamplers((DKO_CommandBuffer *)commandBuffer, DkStage_Compute, firstSlot, bindings, numBindings);
}

static void DEKO3D_BindComputeStorageTextures(SDL_GPUCommandBuffer *commandBuffer, Uint32 firstSlot, SDL_GPUTexture *const *textures, Uint32 numBindings)
{
    DKO_BindStorageTextures((DKO_CommandBuffer *)commandBuffer, DkStage_Compute, firstSlot, textures, numBindings);
}

static void DEKO3D_BindComputeStorageBuffers(SDL_GPUCommandBuffer *commandBuffer, Uint32 firstSlot, SDL_GPUBuffer *const *buffers, Uint32 numBindings)
{
    DKO_BindStorageBuffers((DKO_CommandBuffer *)commandBuffer, DkStage_Compute, firstSlot, buffers, numBindings);
}

static void DEKO3D_PushComputeUniformData(SDL_GPUCommandBuffer *commandBuffer, Uint32 slotIndex, const void *data, Uint32 length)
{
    DKO_PushUniforms((DKO_CommandBuffer *)commandBuffer, DkStage_Compute, slotIndex, data, length);
}

static void DEKO3D_DispatchCompute(SDL_GPUCommandBuffer *commandBuffer, Uint32 groupcountX, Uint32 groupcountY, Uint32 groupcountZ)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    dkCmdBufDispatchCompute(cb->cmd, groupcountX, groupcountY, groupcountZ);
}

static void DEKO3D_DispatchComputeIndirect(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUBuffer *handle, Uint32 offset)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    dkCmdBufDispatchComputeIndirect(cb->cmd, ((DKO_Buffer *)handle)->gpu + offset);
}

static void DEKO3D_EndComputePass(SDL_GPUCommandBuffer *commandBuffer)
{
    ((DKO_CommandBuffer *)commandBuffer)->compute = NULL;
}

// ---------------------------------------------------------------- copies

static Uint32 DKO_BytesPerRow(SDL_GPUTextureFormat format, Uint32 pixels)
{
    Uint32 blockWidth = (Uint32)SDL_max(Texture_GetBlockWidth(format), 1);
    Uint32 blocks = (pixels + blockWidth - 1) / blockWidth;
    return blocks * SDL_GPUTextureFormatTexelBlockSize(format);
}

// SDL's pixels per row and rows per layer as deko3d's DkCopyBuf: the row
// pitch and the layer stride, both in bytes (0 = tightly packed). width is
// the copy's, for a layer stride given without a row length.
static void DKO_CopyPitch(DkCopyBuf *copy, SDL_GPUTextureFormat format, Uint32 pixelsPerRow, Uint32 rowsPerLayer, Uint32 width)
{
    copy->rowLength = pixelsPerRow ? DKO_BytesPerRow(format, pixelsPerRow) : 0;
    copy->imageHeight = 0;
    if (rowsPerLayer) {
        Uint32 blockHeight = (Uint32)SDL_max(Texture_GetBlockHeight(format), 1);
        Uint32 rowBytes = copy->rowLength ? copy->rowLength : DKO_BytesPerRow(format, width);
        copy->imageHeight = (rowsPerLayer + blockHeight - 1) / blockHeight * rowBytes;
    }
}

static void DEKO3D_BeginCopyPass(SDL_GPUCommandBuffer *commandBuffer)
{
    DKO_PassBarrier((DKO_CommandBuffer *)commandBuffer, true);
}

static void DEKO3D_EndCopyPass(SDL_GPUCommandBuffer *commandBuffer)
{
    (void)commandBuffer;
}

static void DEKO3D_UploadToBuffer(SDL_GPUCommandBuffer *commandBuffer, const SDL_GPUTransferBufferLocation *source, const SDL_GPUBufferRegion *destination, bool cycle)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_TransferBuffer *transfer = (DKO_TransferBuffer *)source->transfer_buffer;
    DKO_Buffer *buffer = (DKO_Buffer *)destination->buffer;
    (void)cycle;
    DKO_UseTransfer(cb, transfer);
    dkCmdBufCopyBuffer(cb->cmd, DKO_Gpu(&transfer->alloc) + source->offset, buffer->gpu + destination->offset, destination->size);
}

static void DEKO3D_DownloadFromBuffer(SDL_GPUCommandBuffer *commandBuffer, const SDL_GPUBufferRegion *source, const SDL_GPUTransferBufferLocation *destination)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_TransferBuffer *transfer = (DKO_TransferBuffer *)destination->transfer_buffer;
    DKO_Buffer *buffer = (DKO_Buffer *)source->buffer;
    DKO_UseTransfer(cb, transfer);
    cb->downloads = true;
    dkCmdBufCopyBuffer(cb->cmd, buffer->gpu + source->offset, DKO_Gpu(&transfer->alloc) + destination->offset, source->size);
}

static void DEKO3D_CopyBufferToBuffer(SDL_GPUCommandBuffer *commandBuffer, const SDL_GPUBufferLocation *source, const SDL_GPUBufferLocation *destination, Uint32 size, bool cycle)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    (void)cycle;
    dkCmdBufCopyBuffer(cb->cmd, ((DKO_Buffer *)source->buffer)->gpu + source->offset, ((DKO_Buffer *)destination->buffer)->gpu + destination->offset, size);
}

static void DEKO3D_UploadToTexture(SDL_GPUCommandBuffer *commandBuffer, const SDL_GPUTextureTransferInfo *source, const SDL_GPUTextureRegion *destination, bool cycle)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_TransferBuffer *transfer = (DKO_TransferBuffer *)source->transfer_buffer;
    DKO_Texture *texture = (DKO_Texture *)destination->texture;
    SDL_GPUTextureFormat format = texture->common.info.format;
    (void)cycle;
    DKO_UseTransfer(cb, transfer);
    DkCopyBuf copy;
    copy.addr = DKO_Gpu(&transfer->alloc) + source->offset;
    DKO_CopyPitch(&copy, format, source->pixels_per_row, source->rows_per_layer, destination->w);
    DkImageView view;
    DKO_View(&view, texture, destination->mip_level, 0, false);
    view.layerCount = 0;
    view.type = texture->type == DkImageType_Cubemap ? DkImageType_2DArray : texture->type;
    bool layered = texture->type != DkImageType_3D;
    DkImageRect rect = { destination->x, destination->y, layered ? destination->layer : destination->z, destination->w, destination->h,
                         destination->d ? destination->d : 1 };
    dkCmdBufCopyBufferToImage(cb->cmd, &copy, &view, &rect, 0);
}

static void DEKO3D_DownloadFromTexture(SDL_GPUCommandBuffer *commandBuffer, const SDL_GPUTextureRegion *source, const SDL_GPUTextureTransferInfo *destination)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_TransferBuffer *transfer = (DKO_TransferBuffer *)destination->transfer_buffer;
    DKO_Texture *texture = (DKO_Texture *)source->texture;
    SDL_GPUTextureFormat format = texture->common.info.format;
    DKO_UseTransfer(cb, transfer);
    cb->downloads = true;
    DkCopyBuf copy;
    copy.addr = DKO_Gpu(&transfer->alloc) + destination->offset;
    DKO_CopyPitch(&copy, format, destination->pixels_per_row, destination->rows_per_layer, source->w);
    DkImageView view;
    DKO_View(&view, texture, source->mip_level, 0, false);
    view.layerCount = 0;
    view.type = texture->type == DkImageType_Cubemap ? DkImageType_2DArray : texture->type;
    bool layered = texture->type != DkImageType_3D;
    DkImageRect rect = { source->x, source->y, layered ? source->layer : source->z, source->w, source->h, source->d ? source->d : 1 };
    dkCmdBufCopyImageToBuffer(cb->cmd, &view, &rect, &copy, 0);
}

static void DEKO3D_CopyTextureToTexture(SDL_GPUCommandBuffer *commandBuffer, const SDL_GPUTextureLocation *source, const SDL_GPUTextureLocation *destination,
                                        Uint32 w, Uint32 h, Uint32 d, bool cycle)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_Texture *from = (DKO_Texture *)source->texture;
    DKO_Texture *to = (DKO_Texture *)destination->texture;
    (void)cycle;
    DkImageView fromView, toView;
    DKO_View(&fromView, from, source->mip_level, 0, false);
    DKO_View(&toView, to, destination->mip_level, 0, false);
    fromView.layerCount = toView.layerCount = 0;
    DkImageRect fromRect = { source->x, source->y, from->type == DkImageType_3D ? source->z : source->layer, w, h, d ? d : 1 };
    DkImageRect toRect = { destination->x, destination->y, to->type == DkImageType_3D ? destination->z : destination->layer, w, h, d ? d : 1 };
    dkCmdBufCopyImage(cb->cmd, &fromView, &fromRect, &toView, &toRect, 0);
}

static void DKO_BlitLevel(DKO_CommandBuffer *cb, DKO_Texture *from, Uint32 fromLevel, Uint32 fromLayer, const DkImageRect *fromRect, DKO_Texture *to,
                          Uint32 toLevel, Uint32 toLayer, const DkImageRect *toRect, bool linear)
{
    DkImageView fromView, toView;
    DKO_View(&fromView, from, fromLevel, fromLayer, false);
    DKO_View(&toView, to, toLevel, toLayer, false);
    dkCmdBufBlitImage(cb->cmd, &fromView, fromRect, &toView, toRect, linear ? DkBlitFlag_FilterLinear : 0, 0);
}

static void DEKO3D_GenerateMipmaps(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUTexture *handle)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_Texture *texture = (DKO_Texture *)handle;
    Uint32 layers = texture->type == DkImageType_Cubemap ? 6 : (texture->type == DkImageType_3D ? 1 : texture->common.info.layer_count_or_depth);
    for (Uint32 level = 1; level < texture->common.info.num_levels; level++) {
        for (Uint32 layer = 0; layer < SDL_max(layers, 1u); layer++) {
            DkImageRect from = { 0, 0, 0, SDL_max(texture->common.info.width >> (level - 1), 1u), SDL_max(texture->common.info.height >> (level - 1), 1u), 1 };
            DkImageRect to = { 0, 0, 0, SDL_max(texture->common.info.width >> level, 1u), SDL_max(texture->common.info.height >> level, 1u), 1 };
            DKO_BlitLevel(cb, texture, level - 1, layer, &from, texture, level, layer, &to, true);
        }
        dkCmdBufBarrier(cb->cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    }
}

static void DEKO3D_Blit(SDL_GPUCommandBuffer *commandBuffer, const SDL_GPUBlitInfo *info)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_PassBarrier(cb, false);
    DKO_Texture *from = (DKO_Texture *)info->source.texture;
    DKO_Texture *to = (DKO_Texture *)info->destination.texture;
    if (info->load_op == SDL_GPU_LOADOP_CLEAR) {
        DkImageView view;
        DKO_View(&view, to, info->destination.mip_level, info->destination.layer_or_depth_plane, true);
        DkImageView const *views[1] = { &view };
        dkCmdBufBindRenderTargets(cb->cmd, views, 1, NULL);
        DkScissor whole = { 0, 0, SDL_max(to->common.info.width >> info->destination.mip_level, 1u),
                            SDL_max(to->common.info.height >> info->destination.mip_level, 1u) }; // clears respect it
        dkCmdBufSetScissors(cb->cmd, 0, &whole, 1);
        dkCmdBufClearColorFloat(cb->cmd, 0, DkColorMask_RGBA, info->clear_color.r, info->clear_color.g, info->clear_color.b, info->clear_color.a);
    }
    DkImageRect fromRect = { info->source.x, info->source.y, 0, info->source.w, info->source.h, 1 };
    DkImageRect toRect = { info->destination.x, info->destination.y, 0, info->destination.w, info->destination.h, 1 };
    if (info->flip_mode != SDL_FLIP_NONE) {
        DKO_Unsupported("flipped blits");
    }
    DKO_BlitLevel(cb, from, info->source.mip_level, info->source.layer_or_depth_plane, &fromRect, to, info->destination.mip_level,
                  info->destination.layer_or_depth_plane, &toRect, info->filter == SDL_GPU_FILTER_LINEAR);
}

// ---------------------------------------------------------------- swapchain

static bool DEKO3D_SupportsSwapchainComposition(SDL_GPURenderer *driverData, SDL_Window *window, SDL_GPUSwapchainComposition composition)
{
    (void)driverData;
    (void)window;
    return composition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
}

static bool DEKO3D_SupportsPresentMode(SDL_GPURenderer *driverData, SDL_Window *window, SDL_GPUPresentMode mode)
{
    (void)driverData;
    (void)window;
    return mode == SDL_GPU_PRESENTMODE_VSYNC || mode == SDL_GPU_PRESENTMODE_IMMEDIATE;
}

static bool DEKO3D_ClaimWindow(SDL_GPURenderer *driverData, SDL_Window *window)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    if (renderer->window) {
        return SDL_SetError("deko3d: the Switch has one window, already claimed");
    }
    DKO_WindowData *data = SDL_calloc(1, sizeof(DKO_WindowData));
    if (!data) {
        return false;
    }
    int w = 1280, h = 720;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    data->window = window;
    data->width = (Uint32)w;
    data->height = (Uint32)h;
    data->numImages = 2;

    DkImage const *images[DKO_MAX_SWAPCHAIN_IMAGES];
    for (Uint32 i = 0; i < data->numImages; i++) {
        DKO_Texture *texture = &data->images[i];
        texture->swapchain = true;
        texture->format = DkImageFormat_RGBA8_Unorm;
        texture->type = DkImageType_2D;
        texture->descriptor = ~0u;
        texture->storageDescriptor = ~0u;
        texture->common.info.type = SDL_GPU_TEXTURETYPE_2D;
        texture->common.info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        texture->common.info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        texture->common.info.width = data->width;
        texture->common.info.height = data->height;
        texture->common.info.layer_count_or_depth = 1;
        texture->common.info.num_levels = 1;
        texture->common.info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        DkImageLayoutMaker maker;
        dkImageLayoutMakerDefaults(&maker, renderer->device);
        maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression | DkImageFlags_Usage2DEngine; // blit target
        maker.format = DkImageFormat_RGBA8_Unorm;
        maker.dimensions[0] = data->width;
        maker.dimensions[1] = data->height;
        dkImageLayoutInitialize(&texture->layout, &maker);
        if (!DKO_Allocate(renderer, DKO_HEAP_GPU, (Uint32)dkImageLayoutGetSize(&texture->layout), dkImageLayoutGetAlignment(&texture->layout), &texture->alloc)) {
            SDL_free(data);
            return false;
        }
        dkImageInitialize(&texture->image, &texture->layout, texture->alloc.block->memblock, texture->alloc.offset);
        images[i] = &texture->image;
    }
    DkSwapchainMaker maker;
    dkSwapchainMakerDefaults(&maker, renderer->device, nwindowGetDefault(), images, data->numImages);
    data->swapchain = dkSwapchainCreate(&maker);
    if (!data->swapchain) {
        SDL_free(data);
        return SDL_SetError("deko3d: no swapchain");
    }
    renderer->window = data;
    return true;
}

static void DEKO3D_ReleaseWindow(SDL_GPURenderer *driverData, SDL_Window *window)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    DKO_WindowData *data = renderer->window;
    (void)window;
    if (!data) {
        return;
    }
    dkQueueWaitIdle(renderer->queue);
    dkSwapchainDestroy(data->swapchain);
    for (Uint32 i = 0; i < data->numImages; i++) {
        DKO_FreeNow(renderer, &data->images[i].alloc);
    }
    SDL_free(data);
    renderer->window = NULL;
}

static bool DEKO3D_SetSwapchainParameters(SDL_GPURenderer *driverData, SDL_Window *window, SDL_GPUSwapchainComposition composition, SDL_GPUPresentMode mode)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    (void)window;
    if (composition != SDL_GPU_SWAPCHAINCOMPOSITION_SDR) {
        return SDL_SetError("deko3d: only SDR composition");
    }
    if (renderer->window) {
        dkSwapchainSetSwapInterval(renderer->window->swapchain, mode == SDL_GPU_PRESENTMODE_IMMEDIATE ? 0 : 1);
    }
    return true;
}

static bool DEKO3D_SetAllowedFramesInFlight(SDL_GPURenderer *driverData, Uint32 allowedFramesInFlight)
{
    ((DKO_Renderer *)driverData)->allowedFramesInFlight = allowedFramesInFlight;
    return true;
}

static SDL_GPUTextureFormat DEKO3D_GetSwapchainTextureFormat(SDL_GPURenderer *driverData, SDL_Window *window)
{
    (void)driverData;
    (void)window;
    return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
}

static bool DEKO3D_AcquireSwapchainTexture(SDL_GPUCommandBuffer *commandBuffer, SDL_Window *window, SDL_GPUTexture **swapchainTexture, Uint32 *width, Uint32 *height)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_Renderer *renderer = cb->renderer;
    DKO_WindowData *data = renderer->window;
    (void)window;
    *swapchainTexture = NULL;
    if (!data) {
        return SDL_SetError("deko3d: window not claimed");
    }
    SDL_LockMutex(renderer->submitLock);
    int slot = dkQueueAcquireImage(renderer->queue, data->swapchain); // the queue waits for it
    SDL_UnlockMutex(renderer->submitLock);
    cb->presentWindow = data;
    cb->presentSlot = slot;
    *swapchainTexture = (SDL_GPUTexture *)&data->images[slot];
    if (width) {
        *width = data->width;
    }
    if (height) {
        *height = data->height;
    }
    return true;
}

static bool DEKO3D_WaitForSwapchain(SDL_GPURenderer *driverData, SDL_Window *window)
{
    (void)driverData;
    (void)window;
    return true;
}

static bool DEKO3D_WaitAndAcquireSwapchainTexture(SDL_GPUCommandBuffer *commandBuffer, SDL_Window *window, SDL_GPUTexture **swapchainTexture, Uint32 *width,
                                                  Uint32 *height)
{
    return DEKO3D_AcquireSwapchainTexture(commandBuffer, window, swapchainTexture, width, height);
}

// ---------------------------------------------------------------- submission and fences

static bool DKO_Submit(DKO_CommandBuffer *cb)
{
    DKO_Renderer *renderer = cb->renderer;
    if (cb->downloads) {
        dkCmdBufBarrier(cb->cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache); // so the CPU sees what was written
    }
    DkCmdList list = dkCmdBufFinishList(cb->cmd);
    SDL_LockMutex(renderer->submitLock);
    dkQueueSubmitCommands(renderer->queue, list);
    dkQueueSignalFence(renderer->queue, &cb->fence->fence, false);
    if (cb->presentWindow) {
        dkQueuePresentImage(renderer->queue, cb->presentWindow->swapchain, cb->presentSlot);
    }
    dkQueueFlush(renderer->queue);
    SDL_UnlockMutex(renderer->submitLock);
    dkCmdBufClear(cb->cmd);

    SDL_LockMutex(renderer->stateLock);
    cb->submitted = true;
    DKO_Collect(renderer);
    SDL_UnlockMutex(renderer->stateLock);
    return true;
}

static bool DEKO3D_Submit(SDL_GPUCommandBuffer *commandBuffer)
{
    return DKO_Submit((DKO_CommandBuffer *)commandBuffer);
}

static SDL_GPUFence *DEKO3D_SubmitAndAcquireFence(SDL_GPUCommandBuffer *commandBuffer)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_Fence *fence = cb->fence;
    SDL_AddAtomicInt(&fence->references, 1);
    DKO_Submit(cb);
    return (SDL_GPUFence *)fence;
}

static bool DEKO3D_Cancel(SDL_GPUCommandBuffer *commandBuffer)
{
    DKO_CommandBuffer *cb = (DKO_CommandBuffer *)commandBuffer;
    DKO_Renderer *renderer = cb->renderer;
    dkCmdBufClear(cb->cmd);
    SDL_LockMutex(renderer->stateLock);
    for (Uint32 i = 0; i < renderer->numActive; i++) {
        if (renderer->active[i] == cb) {
            renderer->active[i] = renderer->active[--renderer->numActive];
            break;
        }
    }
    DKO_Recycle(renderer, cb);
    DKO_Collect(renderer);
    SDL_UnlockMutex(renderer->stateLock);
    return true;
}

static bool DEKO3D_Wait(SDL_GPURenderer *driverData)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    SDL_LockMutex(renderer->submitLock);
    dkQueueWaitIdle(renderer->queue);
    SDL_UnlockMutex(renderer->submitLock);
    SDL_LockMutex(renderer->stateLock);
    DKO_Collect(renderer);
    SDL_UnlockMutex(renderer->stateLock);
    return true;
}

static bool DEKO3D_WaitForFences(SDL_GPURenderer *driverData, bool waitAll, SDL_GPUFence *const *fences, Uint32 numFences)
{
    DKO_Renderer *renderer = (DKO_Renderer *)driverData;
    if (waitAll) {
        for (Uint32 i = 0; i < numFences; i++) {
            dkFenceWait(&((DKO_Fence *)fences[i])->fence, -1);
        }
    } else {
        for (;;) {
            bool any = false;
            for (Uint32 i = 0; i < numFences && !any; i++) {
                any = dkFenceWait(&((DKO_Fence *)fences[i])->fence, 0) == DkResult_Success;
            }
            if (any) {
                break;
            }
            svcSleepThread(100000);
        }
    }
    SDL_LockMutex(renderer->stateLock);
    DKO_Collect(renderer);
    SDL_UnlockMutex(renderer->stateLock);
    return true;
}

static bool DEKO3D_QueryFence(SDL_GPURenderer *driverData, SDL_GPUFence *fence)
{
    (void)driverData;
    return dkFenceWait(&((DKO_Fence *)fence)->fence, 0) == DkResult_Success;
}

static void DEKO3D_ReleaseFence(SDL_GPURenderer *driverData, SDL_GPUFence *fence)
{
    (void)driverData;
    DKO_ReleaseFenceReference((DKO_Fence *)fence);
}

// ---------------------------------------------------------------- queries

static bool DEKO3D_SupportsTextureFormat(SDL_GPURenderer *driverData, SDL_GPUTextureFormat format, SDL_GPUTextureType type, SDL_GPUTextureUsageFlags usage)
{
    (void)driverData;
    (void)type;
    if (DKO_ImageFormat(format) == DkImageFormat_None) {
        return false;
    }
    if ((usage & SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET) && !DKO_IsDepthFormat(format)) {
        return false;
    }
    return true;
}

static bool DEKO3D_SupportsSampleCount(SDL_GPURenderer *driverData, SDL_GPUTextureFormat format, SDL_GPUSampleCount count)
{
    (void)driverData;
    (void)format;
    return count <= SDL_GPU_SAMPLECOUNT_8;
}

static SDL_PropertiesID DEKO3D_GetDeviceProperties(SDL_GPUDevice *device)
{
    (void)device;
    static SDL_PropertiesID properties = 0;
    if (!properties) {
        properties = SDL_CreateProperties();
        SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "NVIDIA Tegra X1 (deko3d)");
        SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, "deko3d");
    }
    return properties;
}

// ---------------------------------------------------------------- device

static void DEKO3D_DestroyDevice(SDL_GPUDevice *device)
{
    DKO_Renderer *renderer = (DKO_Renderer *)device->driverData;
    DEKO3D_Wait((SDL_GPURenderer *)renderer);
    if (renderer->window) {
        DEKO3D_ReleaseWindow((SDL_GPURenderer *)renderer, renderer->window->window);
    }
    SDL_LockMutex(renderer->stateLock);
    for (Uint32 i = 0; i < renderer->numReleases; i++) {
        DKO_ApplyRelease(renderer, &renderer->releases[i]);
    }
    renderer->numReleases = 0;
    SDL_UnlockMutex(renderer->stateLock);
    while (renderer->freeCommandBuffers) {
        DKO_CommandBuffer *cb = renderer->freeCommandBuffers;
        renderer->freeCommandBuffers = cb->nextFree;
        dkCmdBufDestroy(cb->cmd);
        SDL_free(cb->chunks);
        SDL_free(cb->transfers);
        SDL_free(cb);
    }
    for (int kind = 0; kind < DKO_HEAP_COUNT; kind++) {
        DKO_Block *block = renderer->heaps[kind].blocks;
        while (block) {
            DKO_Block *next = block->next;
            dkMemBlockDestroy(block->memblock);
            SDL_free(block->free);
            SDL_free(block);
            block = next;
        }
        SDL_DestroyMutex(renderer->heaps[kind].lock);
    }
    dkMemBlockDestroy(renderer->descriptorBlock);
    dkMemBlockDestroy(renderer->codeBlock);
    dkQueueDestroy(renderer->queue);
    dkDeviceDestroy(renderer->device);
    SDL_DestroyMutex(renderer->submitLock);
    SDL_DestroyMutex(renderer->stateLock);
    SDL_free(renderer->freeImageDescriptors);
    SDL_free(renderer->freeSamplerDescriptors);
    SDL_free(renderer->codeFree);
    SDL_free(renderer->freeChunks);
    SDL_free(renderer->active);
    SDL_free(renderer->releases);
    SDL_free(renderer);
    SDL_free(device);
}

static bool DEKO3D_PrepareDriver(SDL_VideoDevice *_this, SDL_PropertiesID props)
{
    (void)_this;
    return SDL_GetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_PRIVATE_BOOLEAN, false);
}

static SDL_GPUDevice *DEKO3D_CreateDevice(bool debugMode, bool preferLowPower, SDL_PropertiesID props)
{
    (void)preferLowPower;
    (void)props;
    DKO_Renderer *renderer = SDL_calloc(1, sizeof(DKO_Renderer));
    if (!renderer) {
        return NULL;
    }
    DkDeviceMaker deviceMaker;
    dkDeviceMakerDefaults(&deviceMaker);
    deviceMaker.cbDebug = debugMode ? DKO_DebugCallback : NULL;
    renderer->device = dkDeviceCreate(&deviceMaker);
    if (!renderer->device) {
        SDL_free(renderer);
        SDL_SetError("deko3d: no device");
        return NULL;
    }
    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, renderer->device);
    queueMaker.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute;
    // Shaders that spill (big translated ones reach 96 registers and want
    // 26-32 KiB per warp) need this much scratch; deko3d's default (2 KiB)
    // traps the draw (GR TRAP at the end of the vertices). Costs this times
    // every warp the GPU runs at once: 8 MiB.
    queueMaker.perWarpScratchMemorySize = DKO_PER_WARP_SCRATCH;
    renderer->queue = dkQueueCreate(&queueMaker);
    renderer->submitLock = SDL_CreateMutex();
    renderer->stateLock = SDL_CreateMutex();
    renderer->allowedFramesInFlight = 2;

    renderer->heaps[DKO_HEAP_GPU].flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    renderer->heaps[DKO_HEAP_UPLOAD].flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    for (int kind = 0; kind < DKO_HEAP_COUNT; kind++) {
        renderer->heaps[kind].lock = SDL_CreateMutex();
    }

    // descriptor sets: images, then samplers, in one block the CPU writes
    Uint32 imageBytes = DKO_MAX_IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor);
    Uint32 samplerBytes = DKO_MAX_SAMPLER_DESCRIPTORS * sizeof(DkSamplerDescriptor);
    DkMemBlockMaker blockMaker;
    dkMemBlockMakerDefaults(&blockMaker, renderer->device, (imageBytes + samplerBytes + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1));
    blockMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    renderer->descriptorBlock = dkMemBlockCreate(&blockMaker);
    renderer->imageDescriptors = dkMemBlockGetGpuAddr(renderer->descriptorBlock);
    renderer->samplerDescriptors = renderer->imageDescriptors + imageBytes;
    renderer->imageDescriptorCpu = (DkImageDescriptor *)dkMemBlockGetCpuAddr(renderer->descriptorBlock);
    renderer->samplerDescriptorCpu = (DkSamplerDescriptor *)((Uint8 *)renderer->imageDescriptorCpu + imageBytes);
    renderer->freeImageDescriptors = SDL_malloc(DKO_MAX_IMAGE_DESCRIPTORS * sizeof(Uint32));
    renderer->freeSamplerDescriptors = SDL_malloc(DKO_MAX_SAMPLER_DESCRIPTORS * sizeof(Uint32));
    for (Uint32 i = 0; i < DKO_MAX_IMAGE_DESCRIPTORS; i++) {
        renderer->freeImageDescriptors[i] = DKO_MAX_IMAGE_DESCRIPTORS - 1 - i;
    }
    for (Uint32 i = 0; i < DKO_MAX_SAMPLER_DESCRIPTORS; i++) {
        renderer->freeSamplerDescriptors[i] = DKO_MAX_SAMPLER_DESCRIPTORS - 1 - i;
    }
    renderer->numFreeImageDescriptors = DKO_MAX_IMAGE_DESCRIPTORS;
    renderer->numFreeSamplerDescriptors = DKO_MAX_SAMPLER_DESCRIPTORS;

    // one code block for every shader: Maxwell addresses them from one base
    dkMemBlockMakerDefaults(&blockMaker, renderer->device, DKO_CODE_SIZE);
    blockMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    renderer->codeBlock = dkMemBlockCreate(&blockMaker);
    DKO_RangeGive(&renderer->codeFree, &renderer->codeFreeCount, &renderer->codeFreeCapacity, 0, DKO_CODE_SIZE - DK_SHADER_CODE_UNUSABLE_SIZE);

    if (!renderer->queue || !renderer->descriptorBlock || !renderer->codeBlock || !renderer->freeImageDescriptors || !renderer->freeSamplerDescriptors) {
        SDL_SetError("deko3d: device setup failed");
        return NULL;
    }

    SDL_GPUDevice *result = SDL_calloc(1, sizeof(SDL_GPUDevice));
    if (!result) {
        return NULL;
    }
    ASSIGN_DRIVER(DEKO3D)
    result->driverData = (SDL_GPURenderer *)renderer;
    result->shader_formats = SDL_GPU_SHADERFORMAT_PRIVATE;
    renderer->sdlDevice = result;
    return result;
}

SDL_GPUBootstrap Deko3DDriver = {
    "deko3d",
    DEKO3D_PrepareDriver,
    DEKO3D_CreateDevice
};

#endif // SDL_GPU_DEKO3D
