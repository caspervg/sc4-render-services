#pragma once

#include <cstdint>

#include "cIGZUnknown.h"
#include "ImGuiServiceIds.h"

struct ImGuiPanelDesc
{
    uint32_t panel_id;
    int32_t order;
    bool visible;
    void (*render)(void* data);
    void (*on_shutdown)(void* data);
    void* data;
};

class cIGZImGuiService : public cIGZUnknown
{
public:
    [[nodiscard]] virtual uint32_t GetServiceID() const = 0;
    [[nodiscard]] virtual uint32_t GetApiVersion() const = 0;
    [[nodiscard]] virtual void* GetContext() const = 0;

    virtual bool RegisterPanel(const ImGuiPanelDesc& desc) = 0;
    virtual bool UnregisterPanel(uint32_t panel_id) = 0;
    virtual bool SetPanelVisible(uint32_t panel_id, bool visible) = 0;
};
