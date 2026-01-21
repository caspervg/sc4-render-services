#pragma once

#include "cIGZUnknown.h"

struct ImGuiPanelDesc
{
    uint32_t id{};
    int32_t order{};
    bool visible{};
    void (*on_init)(void* data){};
    void (*on_render)(void* data){};
    void (*on_update)(void* data){};
    void (*on_visible_changed)(void* data, bool visible){};
    void (*on_shutdown)(void* data){};
    void (*on_unregister)(void* data){};
    void* data{};
    uint32_t fontId{0};  // 0 = use default font
};

struct IDirectDraw7;
struct IDirect3DDevice7;

// Texture handle with generation tracking
struct ImGuiTextureHandle
{
    uint32_t id;              // Unique texture ID
    uint32_t generation;      // Device generation when created
};

// Texture creation descriptor
struct ImGuiTextureDesc
{
    uint32_t width;           // Texture width in pixels
    uint32_t height;          // Texture height in pixels
    const void* pixels;       // RGBA32 source data (required, 4 bytes per pixel)
    bool useSystemMemory;     // Default: false (prefer video memory)
};

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
class cIGZImGuiService : public cIGZUnknown {
public:
    // Returns the service ID (kImGuiServiceID).
    [[nodiscard]] virtual uint32_t GetServiceID() const = 0;

    // Returns the API version (kImGuiServiceApiVersion).
    [[nodiscard]] virtual uint32_t GetApiVersion() const = 0;

    // Returns the ImGui context pointer, or nullptr if not ready.
    [[nodiscard]] virtual void* GetContext() const = 0;

    // Registers a panel; returns false on duplicate ID or missing callbacks.
    virtual bool RegisterPanel(const ImGuiPanelDesc& desc) = 0;

    // Unregisters a panel; returns false if not found.
    virtual bool UnregisterPanel(uint32_t panelId) = 0;

    // Sets a panel's visibility; returns false if not found.
    virtual bool SetPanelVisible(uint32_t panelId, bool visible) = 0;

    // Acquire DX7 interfaces for advanced texture workflows.
    // On success, the service AddRef()'s both interfaces; callers must Release().
    // Prefer acquiring per operation/frame rather than caching across frames.
    virtual bool AcquireD3DInterfaces(IDirect3DDevice7** outD3D, IDirectDraw7** outDD) = 0;

    // Returns true when the DX7 interfaces are ready for use.
    [[nodiscard]] virtual bool IsDeviceReady() const = 0;

    // Generation increments when the DX7 device/context is reinitialized.
    // Callers should rebuild cached textures when this changes.
    [[nodiscard]] virtual uint32_t GetDeviceGeneration() const = 0;

    // Creates a managed texture from RGBA32 pixel data.
    // The service stores source data for automatic recreation after device loss.
    // Returns a handle with the current device generation.
    // Thread safety: Must be called from the render thread only.
    virtual ImGuiTextureHandle CreateTexture(const ImGuiTextureDesc& desc) = 0;

    // Gets a texture ID for use with ImGui::Image().
    // Returns nullptr if handle is invalid or from a stale device generation.
    // The texture surface is recreated on-demand if device was lost.
    // Thread safety: Must be called from the render thread only.
    [[nodiscard]] virtual void* GetTextureID(ImGuiTextureHandle handle) = 0;

    // Releases a texture and frees associated resources.
    // Safe to call with invalid handles (no-op).
    // Thread safety: Must be called from the render thread only.
    virtual void ReleaseTexture(ImGuiTextureHandle handle) = 0;

    // Checks if a texture handle is valid and matches the current device generation.
    // Thread safety: Must be called from the render thread only.
    [[nodiscard]] virtual bool IsTextureValid(ImGuiTextureHandle handle) const = 0;

    // Loads a TTF font from file and registers it with the given ID.
    // size: font size in pixels
    // Returns true on success, false if font cannot be loaded or ID is already registered.
    virtual bool RegisterFont(uint32_t fontId, const char* filePath, float size) = 0;

    // Unregisters a font; returns false if not found.
    virtual bool UnregisterFont(uint32_t fontId) = 0;

    // Gets the ImFont* for a registered font ID, or nullptr if not found.
    [[nodiscard]] virtual void* GetFont(uint32_t fontId) const = 0;
};
