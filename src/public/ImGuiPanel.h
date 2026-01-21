#pragma once

// Base interface for ImGui panels
//
// Lifecycle:
// 1. OnInit() - Called once when panel is registered
// 2. OnUpdate() - Called every frame if visible (before rendering)
// 3. OnRender() - Called every frame if visible (for drawing)
// 4. OnVisibleChanged() - Called when panel visibility changes
// 5. OnUnregister() - Called when manually unregistered (panel stays alive)
// 6. OnShutdown() - Called on service shutdown (panel deletes itself here)
struct ImGuiPanel
{
    virtual ~ImGuiPanel() = default;
    virtual void OnInit() {}
    virtual void OnRender() = 0;
    virtual void OnUpdate() {}
    virtual void OnVisibleChanged(bool) {}
    virtual void OnShutdown() {}
    virtual void OnUnregister() {}
};
