# Power-line pole-style Tab switching + hovering status overlay — design note

Status: implemented (2026-07-05), validated in-game (2026-07-19). Replaces the interim `polestyle`
cheat (now removed) as the real style-switching UX. Two parts: (A) a
Tab/Shift-Tab key hook that cycles the active pole style, and (B) an optional ImGui status overlay
that shows the active style and the live drag heading. Part B degrades to nothing when the ImGui
service (SC4RenderServices / imgui.dll) is not installed — imgui.dll is delay-loaded and no ImGui::
symbol is touched unless the service was acquired, so the DLL loads and every pole feature keeps
working regardless.

## Resolved RE (2026-07-05)

- Keyboard input for all network tools goes through `cSC4ViewInputControlNetworkTool` (one shared
  instance, vtable **`0x00aab008`**). `OnKeyDown` body = **`0x00661e90`**, at vtable slot
  **`0x00aab040`** (= `+0x38`, interface slot 14). Vanilla `OnKeyDown` handles only Escape
  (`0x1b`) — Tab is unclaimed, no conflict.
- The input control stores the **current network type at `+0x50`** (fed to `SL::NetworkManager` in
  `Init`). The power line tool is **network type 5** (`cSC4PowerLineTool` ctor `0x006503c0` calls the
  `cSC4NetworkTool` base with 5). This enum is the runtime discriminator.
- **Verified in-game (2026-07-05):** the `+0x4c` pointer is a shared `cSC4NetworkToolUI` (vtable
  `0x00aa9140`), **not** the power tool object — an initial vtable-compare gate against the power
  tool's `0x00aa9f30` therefore read `powerActive=false` and was wrong. The network-type enum is
  both correct and more rebuild-stable than any address.
- Gate = `*(control) == 0x00aab008 && *(uint32*)(control + 0x50) == 5`. The one address
  (`0x00aab008`) is byte-validated at install (the OnKeyDown slot patch at `0x00aab040` confirms it
  transitively), so the overlay only registers when it is confirmed on the live binary. On a
  recompiled/other build the byte-check fails → no Tab, no overlay, no crash — a self-validating gate
  that does not rely on the version string alone.

## Decisions locked (from review Q&A, 2026-07-05)

- **Key capture:** RE + patch the tool's `cISC4ViewInputControl::OnKeyDown` vtable slot. The power
  tool is a *subtool* sharing the `cSC4NetworkTool` input path, so the patch is **not** auto-scoped
  the way the `DrawNetworkLine` slot patch was — it must fire only when the active network subtool
  is the power line tool (network type 5). See "Scoping" below.
- **What Tab changes:** Tab / Shift-Tab cycle `gActiveStyleIndex` through vanilla + every loaded
  `[PowerPoles.<Name>]` family, forward / backward. FAR ratio stays fully automatic (chosen by drag
  angle, unchanged). The overlay only *displays* the auto-detected ratio/angle.
- **Overlay contents:** active style name, live drag angle/ratio, Shift-constraint state +
  inter-pole distance, a dimmed hotkey-hint footer, and a dimmed summary of the selected style's
  capabilities (which FAR ratios it defines). Capability presentation is the one open UX question
  (see "Open question").
- **Overlay visibility:** whenever the power-line tool is the active view input control. Blank/idle
  drag fields when not dragging.

---

## Part A — Tab / Shift-Tab pole-style switch

### A.1 What receives the key

The concrete tool implements `cISC4ViewInputControl` (`GZIID 0xC6BB8345`, see
`vendor/gzcom-dll/.../cISC4ViewInputControl.h`). Keyboard keys arrive at:

```
virtual bool OnKeyDown(int32_t vkCode, uint32_t modifiers)   // interface slot 14
virtual bool OnKeyUp  (int32_t vkCode, uint32_t modifiers)   // interface slot 15
```

Slot index counts `cIGZUnknown`'s `QueryInterface/AddRef/Release` as 0/1/2, then `Init`(3),
`Shutdown`(4), `GetID`(5), `SetID`(6), `GetCursor`(7), `SetCursor`x2 (8,9), `SetWindow`(10),
`IsSelfScrollingView`(11), `ShouldStack`(12), `OnCharacter`(13), **`OnKeyDown`(14)**, `OnKeyUp`(15),
mouse handlers (16+). So `OnKeyDown` is at **byte offset `0x38`** within the vtable that exposes the
`cISC4ViewInputControl` interface. `modifiers` carries the framework Shift/Ctrl/Alt flags (same
`MOD_SHIFT`/`MOD_CONTROL` bits plop-and-paint reads); Shift-Tab is therefore visible in one call.

### A.2 Which vtable — resolved: shared input control, gating required

Keyboard input is not handled by `cSC4PowerLineTool` directly. It is handled by
**`cSC4ViewInputControlNetworkTool`**, the single view-input-control class shared by *every* network
subtool (road/rail/street/power) — the power line tool is a subtool of it. Its vtable starts at
**`0x00aab008`** (confirmed). Therefore:

- **`OnKeyDown` slot to patch = `0x00aab008 + 0x38 = 0x00aab040`** (verify the dword there is the
  real `OnKeyDown` body via disassembly + `patch.GetOriginalTarget()` byte-check before committing,
  same discipline as every other patch in the DLL).
- Because this vtable is **shared across all network tools**, the patch is **not** auto-scoped the
  way the `DrawNetworkLine` slot patch (`0x00aa9f78`, in the power tool's *own* primary vtable
  `0x00aa9f30`) was. A raw patch would also intercept Tab while dragging roads/rail/street. **Runtime
  gating is mandatory.**

**Gating recipe (resolved 2026-07-05).** In the hook, `this` is the `cSC4ViewInputControlNetworkTool`
instance, which **caches the current network type at `+0x50`** (the value it feeds to
`SL::NetworkManager` in `Init`). Gate on `*(uint32*)(this + 0x50) == 5`; forward to the original
untouched otherwise.

The initially-planned alternative — reading the active subtool at `+0x4c` and comparing its vtable to
the power tool's `0x00aa9f30` — was tried and **rejected in-game**: `+0x4c` is a shared
`cSC4NetworkToolUI` (vtable `0x00aa9140`), not the power tool object, so that compare always read
false. The `+0x50` network-type enum is both correct and more rebuild-stable than any address.

### A.3 Hook behavior

```
uint8_t __fastcall OnKeyDownHook(void* control, void* edx, int vkCode, uint32_t modifiers):
    // Bail to original unless Tab AND the active network type is power (5).
    if (enabled and vkCode == VK_TAB and *(control) == 0x00aab008 and *(control+0x50) == 5):
        shift = GetAsyncKeyState(VK_SHIFT) & 0x8000
        CycleStyle(shift ? -1 : +1)
        return 1                      // consume; do not let vanilla see Tab
    return original(control, vkCode, modifiers)
```

`CycleStyle(dir)` advances `gActiveStyleIndex` over `[0 .. gStyles.size()]` (0 = vanilla), wrapping,
exactly like the current `kCheatPoleStyle` handler — reuse that logic, just driven by the key. It
already only affects newly-placed poles, which is the intended semantics.

Notes / risks:
- **Reuse `VTableSlotPatch`** (already in the DLL for `DrawNetworkLine` and the shutdown observer).
  The hook must preserve the original `this` and, for case 2, call through to the original for
  non-power tools and non-Tab keys so all other input still works.
- **Calling convention:** `__thiscall` (this in ECX; `vkCode`,`modifiers` on stack). Return `bool`
  in AL. Returning `true` consumes the key so vanilla doesn't also act on Tab.
- **Tab today:** confirm vanilla doesn't already bind Tab inside the power tool (it likely doesn't —
  Tab is a UI focus key). If it does, the consume-on-power-tool behavior overrides it only while the
  power tool is active, which is acceptable and reversible.
- **Shift interaction:** Shift already means "constrain to regular headings" during a FAR drag
  (that path reads Shift independently in the `DrawNetworkLine` hook). Shift-Tab is a discrete
  keypress, not a held-during-drag modifier, so the two uses don't collide. Document it anyway.

### A.4 `polestyle` cheat

Removed (2026-07-05). Tab/Shift-Tab is the sole style-switching UX; `CycleStyle()` is now driven
only by the key hook.

---

## Part B — Optional hovering status overlay

### B.1 Crash-safe optionality (hard requirement)

The overlay lives **inside `PowerPoleCustomizationDirector`** (it already owns all the state the
overlay shows as file-local globals — no cross-DLL data plumbing needed). It consumes the ImGui
system service the same way the sample directors do:

```cpp
if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                   reinterpret_cast<void**>(&imguiService_))) {
    LOG_INFO("PowerPoleCustomization: ImGui service absent; status overlay disabled.");
    // everything else (key hook, pole logic) continues normally
}
```

`GetSystemService` returning false when SC4RenderServices/the ImGui service DLL is not installed is
the entire crash-safety story: no service → no `RegisterPanel` → no overlay, and every pole feature
still works. Guard `RegisterPanel` / `UnregisterPanel` / `Release` on the pointer being non-null.
Do **not** link the overlay into the pole hooks in any way that a missing service could break.

### B.2 Panel registration

Register in `PostAppInit` (after pole hooks install), unregister + `Release` in `PostAppShutdown`.
Use a dedicated panel id (e.g. `0xB07E2000`, in this DLL's existing `0xB07E****` block). Follow the
`ImGuiPanelDesc` free-function-callback pattern from `ImGuiSampleCityDirector.cpp` (or the
`ImGuiPanelAdapter<T>` template) — `on_render` + `on_shutdown` are the only required callbacks.

### B.3 Visibility gate — "power tool active"

Detect in `on_update` (runs each frame before render) by querying the current view input control and
testing tool identity, then set the panel's internal `visible_` (the render callback early-returns
when false — the plop-and-paint `PaintStatusPanel` pattern):

- Get `cISC4View3DWin` (via `cISC4App` → `GetView3DWin`, as plop-and-paint's director does with
  `pView3D_`), call `GetCurrentViewInputControl()`, and test whether it is the power tool.
- **Reuse the exact tool-identity check developed for the key hook** (network type `== 5` at
  `+0x35c`, adjusting for the interface subobject offset) so there is one source of truth. If the
  current control isn't the power tool, hide the overlay.

### B.4 Contents and data sources

All values are read from the existing DLL globals — display only. Style after the plop-and-paint
`PaintStatusPanel`: `SetNextWindowBgAlpha(0.7f)`, flags
`NoTitleBar|NoResize|AlwaysAutoResize|NoFocusOnAppearing|NoNav|NoInputs`, window name `##PoleStyle`.
Add `SetNextWindowPos({margin, margin}, ImGuiCond_Always)` for the requested top-left placement
(e.g. `ImVec2(10,10)`), or `ImGuiCond_FirstUseEver` if we want it draggable.

| Line | Source | Example |
|---|---|---|
| Style name + index | `gActiveStyleIndex`, `gStyles[i].name` | `Style: Steel (2 / 3)` or `Style: Vanilla` |
| Live drag heading | `gFarDragActive`, `gFarDragRatioIndex`, `gFarDragOrient`, `kFarRatioLabels`, `kFarRatios` | `Drag: FA-2 (2:1 — 26.6°)` / `Drag: 45° diagonal` / `Drag: orthogonal` / `Drag: —` when idle |
| Shift-constraint | the same Shift signal the FAR drag hook reads | `Shift: regular headings only` (only when held) |
| Inter-pole distance | active `PoleStyle.maxCellsBetweenPoles` (else vanilla 10) | `Poles every 14 cells` |
| Capabilities (dimmed) | active `PoleStyle.hasFarHeading[]` | see open question |
| Hotkey footer (dimmed) | static | `Tab / Shift+Tab  style     Shift  constrain` |

Compute the drag-angle degrees from `kFarRatios[gFarDragRatioIndex]` (`atan2(rise,run)`) for the
label, matching what is currently only written to the log. The `2:1 — 26.6°` phrasing is the
author-facing form the FAR design note already prescribes.

### B.5 Capabilities summary — open UX question

The active `PoleStyle` knows which FAR headings it actually defines (`hasFarHeading[16]`, indexed
`ratioIndex*4 + orient`) and which regular masks it overrides (`hasMask[16]`). Presentation (decided 2026-07-05): a compact dimmed row of the supported ratios, labelled
`FA-<ratio>`, e.g. `FA: FA-2  FA-3  FA-6` (collapse the 4 XP/XN/ZP/ZN orientations per ratio; show a
ratio if any orientation is defined). Highlight the ratio the current drag is near when a FAR drag
is active. Deferred (only if authors ask): a per-orientation XP/XN/ZP/ZN coverage indicator.

**Label convention.** `FAR` (Fractionally Angled *Roads*) is retained only as the internal code
name — it is already pervasive across the DLL and RE docs. All *player-facing* text uses `FA-`:
`FA-2` (2:1, 26.6°), `FA-3` (3:1, 18.4°), `FA-6` (6:1, 9.5°), `FA-1.5` (3:2, 33.7°). The live
drag-heading line may pair it with the slope/angle, e.g. `Drag: FA-2 (2:1 — 26.6°)`.

### B.6 Thread-safety

The globals are written on the main/sim thread (key hook, `DrawNetworkLine` hook) and read on the
render thread inside the ImGui callback. All fields are scalars/enums read for display; torn reads
are benign (worst case: one stale frame). If we want to be tidy, snapshot the handful of values into
a small POD under a lightweight guard in `on_update` and render from the snapshot. Not a correctness
requirement — call out in code, don't over-engineer.

---

## Files & symbols

- `src/sample/power-pole-customization/PowerPoleCustomizationDirector.cpp` — add `OnKeyDown` hook +
  `VTableSlotPatch`; add the ImGui panel (render/update/shutdown), service acquisition in
  `PostAppInit`, teardown in `PostAppShutdown`; factor the existing `polestyle` cycle into a shared
  `CycleStyle(dir)`; add a tool-identity helper (network type at input-control `+0x50` == 5).
- New constants: `kNetworkToolInputControlVtable = 0x00aab008`, `kOnKeyDownSlot = 0x00aab040`
  (= vtable + `0x38`), a `VK_TAB` handler, `kToolNetworkType_Power = 5`,
  `kTool_NetworkType = 0x35c` (on the driven `cSC4NetworkTool`), `kStatusPanelId`.
- Docs to update on completion: `docs/power-pole-customization.md` (document Tab UX + overlay),
  `docs/sc4-powerline-tool-re.md` (record the resolved `OnKeyDown` slot/vtable and the case-1/2
  finding), and the FAR design note's "not yet done" line.

## Validation

1. Key hook installs (byte-verified target) and Tab cycles the logged active style while the power
   tool is active; Tab does nothing (passes through) under road/rail/street — the scoping test.
2. Shift-Tab cycles backward; wraps at both ends including vanilla(0).
3. Overlay: appears only with the power tool active; shows correct style, live ratio/angle during a
   FAR drag, Shift line only when Shift held, correct inter-pole distance per style.
4. Uninstall SC4RenderServices / ImGui service DLL → game loads, power tool + Tab still fully work,
   no overlay, no crash, one info log line.
5. Device-loss (zoom/resolution change) doesn't break the overlay (no textures used, so trivially
   fine — but confirm no stale service pointer after `GetDeviceGeneration` bumps).

## Sequencing

Part A (key hook) is independently valuable and testable via the log alone — land it first, it
removes the `polestyle`-cheat crutch. Part B rides on A's tool-identity check and adds the visible
polish. Both depend on the one RE deliverable in A.2 (resolve OnKeyDown vtable + scoping).
