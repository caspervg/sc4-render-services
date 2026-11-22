SC4 Render Services

This release archive contains:
- the Windows installer
- SC4RenderServices.dll, the same DLL the installer places, to check with
  `gh attestation verify SC4RenderServices.dll --repo caspervg/sc4-render-services`
- LICENSE.txt
- THIRD_PARTY_NOTICES.txt
- SC4RenderServices.spdx.json, the SBOM

Installation:
1. Run the included installer.
2. Select your SimCity 4 game root (the folder that contains "Apps"). The installer only uses it
   to check the game version.
3. Select your SimCity 4 Plugins folder.
4. The installer places SC4RenderServices.dll in the Plugins folder.
5. If SC4RenderServices.ini does not already exist, the installer adds a default copy in the Plugins folder.

Notes:
- Works with the game's built-in DirectX driver, SCD3D11 and scvk.
- imgui.dll in the game "Apps" folder is no longer used. Older versions installed it there. Delete it
  once all your plugins that use SC4 Render Services are updated to builds for this version.
- Existing SC4RenderServices.ini files are left in place so local configuration is preserved.
- This release archive does not include the sample DLLs. Build from source if you need them.

Source and documentation:
- Full project documentation is in README.md in the repository.
