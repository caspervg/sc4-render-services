#pragma once

#include "cIGZUnknown.h"

struct ImGuiPanelDesc
{
    uint32_t id;
    int32_t order;
    bool visible;
    void (*on_init)(void* data);
    void (*on_render)(void* data);
    void (*on_update)(void* data);
    void (*on_visible_changed)(void* data, bool visible);
    void (*on_shutdown)(void* data);
    void (*on_unregister)(void* data);
    void* data;
};

struct IDirectDraw7;
struct IDirect3DDevice7;

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
class cIGZImGuiService : public cIGZUnknown
{
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
};
