#pragma once

#include <type_traits>

#include "public/cIGZImGuiService.h"
#include "public/ImGuiPanel.h"

template <typename T,
          std::enable_if_t<std::is_base_of_v<ImGuiPanel, T>, int> = 0>
struct ImGuiPanelAdapter
{
    static void OnInit(void* data)
    {
        static_cast<T*>(data)->OnInit();
    }

    static void OnRender(void* data)
    {
        static_cast<T*>(data)->OnRender();
    }

    static void OnUpdate(void* data)
    {
        static_cast<T*>(data)->OnUpdate();
    }

    static void OnVisibleChanged(void* data, bool visible)
    {
        static_cast<T*>(data)->OnVisibleChanged(visible);
    }

    static void OnShutdown(void* data)
    {
        static_cast<T*>(data)->OnShutdown();
    }

    static void OnUnregister(void* data)
    {
        static_cast<T*>(data)->OnUnregister();
    }

    static ImGuiPanelDesc MakeDesc(T* instance, const uint32_t panelId, const int32_t order, const bool visible, const uint32_t fontId) {
        ImGuiPanelDesc desc{};
        desc.id = panelId;
        desc.order = order;
        desc.visible = visible;
        desc.on_init = &OnInit;
        desc.on_render = &OnRender;
        desc.on_update = &OnUpdate;
        desc.on_visible_changed = &OnVisibleChanged;
        desc.on_shutdown = &OnShutdown;
        desc.on_unregister = &OnUnregister;
        desc.data = instance;
        desc.fontId = fontId;
        return desc;
    }

    static ImGuiPanelDesc MakeDesc(T* instance, const uint32_t panelId, const int32_t order, const bool visible)
    {
        return MakeDesc(instance, panelId, order, visible, 0);
    }

    static ImGuiPanelDesc MakeDesc(T* instance, const uint32_t panelId, const int32_t order)
    {
        return MakeDesc(instance, panelId, order, true);
    }

    static ImGuiPanelDesc MakeDesc(T* instance, const uint32_t panelId)
    {
        return MakeDesc(instance, panelId, 0);
    }
};
