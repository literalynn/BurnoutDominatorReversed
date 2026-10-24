#include "runtime/shared_frames.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
    enum class Owner
    {
        Free,
        Backend, // being written
        Pending, // published, not taken yet
        Window,  // shown, or until the GPU has finished drawing it
    };
    struct Slot
    {
        Owner owner = Owner::Free;
        intptr_t memory = 0; // a new image's memory handle, until the window takes it
        uint64_t bytes = 0;
        uint32_t width = 0, height = 0;
    };
    std::mutex g_mutex;
    Slot g_slots[ps2x::shared_frames::kSlots];
    int g_pending = -1;
    intptr_t g_pendingReady = 0;
    bool g_active = false;
    bool g_hasLuid = false;
    uint8_t g_luid[8] = {};

    void closeHandle(intptr_t handle)
    {
#ifdef _WIN32
        if (handle)
            CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
        (void)handle;
#endif
    }
}

namespace ps2x::shared_frames
{
    void setDeviceLuid(const uint8_t luid[8])
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::memcpy(g_luid, luid, sizeof(g_luid));
        g_hasLuid = true;
    }

    int beginFrame()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active)
            return -1;
        for (int i = 0; i < kSlots; ++i)
            if (g_slots[i].owner == Owner::Free)
            {
                g_slots[i].owner = Owner::Backend;
                return i;
            }
        // The window has not taken the last frame: this one replaces it.
        const int slot = g_pending;
        if (slot >= 0)
        {
            closeHandle(g_pendingReady);
            g_pendingReady = 0;
            g_pending = -1;
            g_slots[slot].owner = Owner::Backend;
        }
        return slot;
    }

    void setImage(int slot, intptr_t memory, uint64_t bytes, uint32_t width, uint32_t height)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Slot &s = g_slots[slot];
        closeHandle(s.memory); // an image the window never took
        s.memory = memory;
        s.bytes = bytes;
        s.width = width;
        s.height = height;
    }

    void publish(int slot, intptr_t ready)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active)
        {
            closeHandle(ready);
            g_slots[slot].owner = Owner::Free;
            return;
        }
        if (g_pending >= 0) // a frame the window has not taken: this one replaces it
        {
            closeHandle(g_pendingReady);
            g_slots[g_pending].owner = Owner::Free;
        }
        g_slots[slot].owner = Owner::Pending;
        g_pending = slot;
        g_pendingReady = ready;
    }

    void release(int slot)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_slots[slot].owner = Owner::Free;
    }
}

#ifdef _WIN32
// raylib's GLFW loads OpenGL for the window; rlgl has none of the entry points below.
extern "C"
{
    typedef void (*GLFWglproc)(void);
    GLFWglproc glfwGetProcAddress(const char *procname);
}

namespace
{
    using GLenum = unsigned int;
    using GLuint = unsigned int;
    using GLint = int;
    using GLsizei = int;
    using GLbitfield = unsigned int;
    using GLuint64 = uint64_t;
    using GLubyte = unsigned char;
    using GLsync = struct __GLsync *;

    constexpr GLenum kTexture2D = 0x0DE1, kRgba8 = 0x8058, kNumExtensions = 0x821D, kExtensions = 0x1F03, kVendor = 0x1F00;
    constexpr GLenum kDedicatedMemoryObject = 0x9581, kHandleTypeOpaqueWin32 = 0x9587;
    constexpr GLenum kLayoutShaderReadOnly = 0x9591, kDeviceLuid = 0x9599;
    constexpr GLenum kSyncGpuCommandsComplete = 0x9117, kAlreadySignaled = 0x911A, kConditionSatisfied = 0x911C;

    struct
    {
        void(APIENTRY *GetIntegerv)(GLenum, GLint *);
        const GLubyte *(APIENTRY *GetString)(GLenum);
        const GLubyte *(APIENTRY *GetStringi)(GLenum, GLuint);
        GLenum(APIENTRY *GetError)();
        void(APIENTRY *GenTextures)(GLsizei, GLuint *);
        void(APIENTRY *DeleteTextures)(GLsizei, const GLuint *);
        void(APIENTRY *BindTexture)(GLenum, GLuint);
        void(APIENTRY *CreateMemoryObjectsEXT)(GLsizei, GLuint *);
        void(APIENTRY *DeleteMemoryObjectsEXT)(GLsizei, const GLuint *);
        void(APIENTRY *MemoryObjectParameterivEXT)(GLuint, GLenum, const GLint *);
        void(APIENTRY *ImportMemoryWin32HandleEXT)(GLuint, GLuint64, GLenum, void *);
        void(APIENTRY *TexStorageMem2DEXT)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
        void(APIENTRY *GetUnsignedBytevEXT)(GLenum, GLubyte *);
        void(APIENTRY *GenSemaphoresEXT)(GLsizei, GLuint *);
        void(APIENTRY *DeleteSemaphoresEXT)(GLsizei, const GLuint *);
        void(APIENTRY *ImportSemaphoreWin32HandleEXT)(GLuint, GLenum, void *);
        void(APIENTRY *WaitSemaphoreEXT)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *);
        GLsync(APIENTRY *FenceSync)(GLenum, GLbitfield);
        GLenum(APIENTRY *ClientWaitSync)(GLsync, GLbitfield, GLuint64);
        void(APIENTRY *DeleteSync)(GLsync);
    } gl{};

    template <typename Fn>
    bool load(Fn &fn, const char *name)
    {
        fn = reinterpret_cast<Fn>(glfwGetProcAddress(name));
        return fn != nullptr;
    }

    bool hasExtension(const char *name)
    {
        GLint count = 0;
        gl.GetIntegerv(kNumExtensions, &count);
        for (GLint i = 0; i < count; ++i)
            if (const auto *ext = reinterpret_cast<const char *>(gl.GetStringi(kExtensions, GLuint(i))); ext && std::strcmp(ext, name) == 0)
                return true;
        return false;
    }

    struct WindowSlot
    {
        GLuint texture = 0, memory = 0;
        uint32_t width = 0, height = 0;
        GLsync drawn = nullptr; // after the last commands that drew it
    };
    WindowSlot g_window[ps2x::shared_frames::kSlots];
    int g_shown = -1;
    bool g_importing = false;
}

namespace ps2x::shared_frames
{
    bool startWindow()
    {
        const char *setting = std::getenv("BDR_SHARED_FRAMES");
        if (setting && *setting == '0')
            return false;
        const bool loaded =
            load(gl.GetIntegerv, "glGetIntegerv") && load(gl.GetString, "glGetString") &&
            load(gl.GetStringi, "glGetStringi") && load(gl.GetError, "glGetError") &&
            load(gl.GenTextures, "glGenTextures") && load(gl.DeleteTextures, "glDeleteTextures") &&
            load(gl.BindTexture, "glBindTexture") && load(gl.FenceSync, "glFenceSync") &&
            load(gl.ClientWaitSync, "glClientWaitSync") && load(gl.DeleteSync, "glDeleteSync");
        if (!loaded || !hasExtension("GL_EXT_memory_object") || !hasExtension("GL_EXT_memory_object_win32") ||
            !hasExtension("GL_EXT_semaphore") || !hasExtension("GL_EXT_semaphore_win32") ||
            !load(gl.CreateMemoryObjectsEXT, "glCreateMemoryObjectsEXT") ||
            !load(gl.DeleteMemoryObjectsEXT, "glDeleteMemoryObjectsEXT") ||
            !load(gl.MemoryObjectParameterivEXT, "glMemoryObjectParameterivEXT") ||
            !load(gl.ImportMemoryWin32HandleEXT, "glImportMemoryWin32HandleEXT") ||
            !load(gl.TexStorageMem2DEXT, "glTexStorageMem2DEXT") || !load(gl.GetUnsignedBytevEXT, "glGetUnsignedBytevEXT") ||
            !load(gl.GenSemaphoresEXT, "glGenSemaphoresEXT") || !load(gl.DeleteSemaphoresEXT, "glDeleteSemaphoresEXT") ||
            !load(gl.ImportSemaphoreWin32HandleEXT, "glImportSemaphoreWin32HandleEXT") ||
            !load(gl.WaitSemaphoreEXT, "glWaitSemaphoreEXT"))
        {
            std::printf("[gs] frames go through host memory: OpenGL cannot import Vulkan images\n");
            return false;
        }
        // shortcut: tested on NVIDIA only, where the picture was checked; others keep host memory
        // unless BDR_SHARED_FRAMES=1, until someone checks the picture on their drivers.
        const char *vendor = reinterpret_cast<const char *>(gl.GetString(kVendor));
        if (!(setting && *setting == '1') && !(vendor && std::strstr(vendor, "NVIDIA")))
        {
            std::printf("[gs] frames go through host memory: frames shared with OpenGL are only tested on NVIDIA "
                        "(BDR_SHARED_FRAMES=1 tries them)\n");
            return false;
        }
        GLubyte luid[8] = {};
        gl.GetUnsignedBytevEXT(kDeviceLuid, luid);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_hasLuid || std::memcmp(luid, g_luid, sizeof(luid)) != 0)
        {
            std::printf("[gs] frames go through host memory: %s\n",
                        g_hasLuid ? "the window's OpenGL context is on another GPU" : "no Vulkan device to share them");
            return false;
        }
        g_active = g_importing = true;
        std::printf("[gs] frames stay on the GPU: the window draws the GS images\n");
        return true;
    }

    void stopWindow()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_active = g_importing = false;
        closeHandle(g_pendingReady);
        g_pendingReady = 0;
        if (g_pending >= 0)
            g_slots[g_pending].owner = Owner::Free;
        g_pending = -1;
    }

    void releaseDrawn()
    {
        if (!g_importing)
            return;
        for (int i = 0; i < kSlots; ++i)
            if (WindowSlot &s = g_window[i]; s.drawn)
                if (const GLenum state = gl.ClientWaitSync(s.drawn, 0, 0); state == kAlreadySignaled || state == kConditionSatisfied)
                {
                    gl.DeleteSync(s.drawn);
                    s.drawn = nullptr;
                    release(i);
                }
    }

    bool showNewFrame(ShownFrame &out)
    {
        if (!g_importing)
            return false;
        releaseDrawn();
        int slot;
        intptr_t ready, memory;
        uint64_t bytes;
        uint32_t width, height;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_pending < 0)
                return false;
            slot = g_pending;
            Slot &s = g_slots[slot];
            ready = g_pendingReady;
            memory = s.memory;
            bytes = s.bytes;
            width = s.width;
            height = s.height;
            s.memory = 0;
            s.owner = Owner::Window;
            g_pending = -1;
            g_pendingReady = 0;
        }
        for (int i = 0; i < 8 && gl.GetError() != 0; ++i) // errors left by earlier calls
        {
        }
        WindowSlot &s = g_window[slot];
        if (memory)
        {
            if (s.texture)
                gl.DeleteTextures(1, &s.texture);
            if (s.memory)
                gl.DeleteMemoryObjectsEXT(1, &s.memory);
            const GLint dedicated = 1; // Granite gives external images allocations of their own
            gl.CreateMemoryObjectsEXT(1, &s.memory);
            gl.MemoryObjectParameterivEXT(s.memory, kDedicatedMemoryObject, &dedicated);
            gl.ImportMemoryWin32HandleEXT(s.memory, bytes, kHandleTypeOpaqueWin32, reinterpret_cast<void *>(memory));
            gl.GenTextures(1, &s.texture);
            gl.BindTexture(kTexture2D, s.texture);
            gl.TexStorageMem2DEXT(kTexture2D, 1, kRgba8, GLsizei(width), GLsizei(height), s.memory, 0);
            gl.BindTexture(kTexture2D, 0);
            closeHandle(memory); // after the storage: NVIDIA's Windows driver uses the handle until then
            s.width = width;
            s.height = height;
        }
        GLuint semaphore = 0;
        gl.GenSemaphoresEXT(1, &semaphore);
        gl.ImportSemaphoreWin32HandleEXT(semaphore, kHandleTypeOpaqueWin32, reinterpret_cast<void *>(ready));
        closeHandle(ready);
        const GLenum layout = kLayoutShaderReadOnly; // the backend's release barrier
        gl.WaitSemaphoreEXT(semaphore, 0, nullptr, 1, &s.texture, &layout);
        gl.DeleteSemaphoresEXT(1, &semaphore);
        // The frame shown until now goes back once the GPU is past this. Placed before the wait,
        // which it need not follow, slots came back sooner and a race ran 4% slower at 240 Hz.
        if (g_shown >= 0)
            g_window[g_shown].drawn = gl.FenceSync(kSyncGpuCommandsComplete, 0);
        if (const GLenum error = gl.GetError(); error != 0) // the fence's too
        {
            std::printf("[gs] frames go through host memory from now on: OpenGL error 0x%x importing one\n", error);
            stopWindow();
            return false;
        }
        g_shown = slot;
        out = {s.texture, s.width, s.height};
        return true;
    }

    bool shownFrame(ShownFrame &out)
    {
        if (g_shown < 0)
            return false;
        out = {g_window[g_shown].texture, g_window[g_shown].width, g_window[g_shown].height};
        return true;
    }
}
#else
namespace ps2x::shared_frames
{
    bool startWindow() { return false; }
    void stopWindow() {}
    bool showNewFrame(ShownFrame &) { return false; }
    void releaseDrawn() {}
    bool shownFrame(ShownFrame &) { return false; }
}
#endif
