// Sample demonstrating safe texture management with the ImGui service.
// This shows how to create textures, handle device loss, and use the RAII wrapper.

#include "cIGZFrameWork.h"
#include "cRZBaseSystemService.h"
#include "cRZCOMDllDirector.h"
#include "GZServPtrs.h"
#include "public/cIGZImGuiService.h"
#include "public/ImGuiServiceIds.h"
#include "public/ImGuiTexture.h"
#include "utils/Logger.h"

#include "imgui.h"

#include <vector>

namespace {
    // Generate a simple test pattern (checkerboard)
    std::vector<uint8_t> GenerateTestPattern(uint32_t width, uint32_t height) {
        // Prevent integer overflow
        const size_t pixelCount = static_cast<size_t>(width) * height;
        if (pixelCount > SIZE_MAX / 4) {
            return {};  // Return empty vector on overflow
        }
        
        std::vector<uint8_t> pixels(pixelCount * 4);  // RGBA32
        
        for (uint32_t y = 0; y < height; ++y) {
            size_t rowOffset = static_cast<size_t>(y) * width * 4;
            for (uint32_t x = 0; x < width; ++x) {
                const size_t offset = rowOffset + x * 4;
                const bool isWhite = ((x / 16) + (y / 16)) % 2 == 0;
                
                if (isWhite) {
                    pixels[offset + 0] = 255;  // R
                    pixels[offset + 1] = 255;  // G
                    pixels[offset + 2] = 255;  // B
                    pixels[offset + 3] = 255;  // A
                } else {
                    pixels[offset + 0] = 64;   // R
                    pixels[offset + 1] = 64;   // G
                    pixels[offset + 2] = 64;   // B
                    pixels[offset + 3] = 255;  // A
                }
            }
        }
        
        return pixels;
    }

    // Sample panel data
    struct TextureSampleData {
        cIGZImGuiService* service;
        ImGuiTexture texture1;
        ImGuiTexture texture2;
        uint32_t lastDeviceGeneration;
        bool texturesCreated;
        
        TextureSampleData()
            : service(nullptr)
            , lastDeviceGeneration(0)
            , texturesCreated(false) {}
    };

    void OnInit(void* data) {
        auto* sampleData = static_cast<TextureSampleData*>(data);
        LOG_INFO("ImGuiTextureSample: OnInit called");
        
        if (!sampleData->service) {
            return;
        }

        // Create test textures
        auto pattern1 = GenerateTestPattern(128, 128);
        auto pattern2 = GenerateTestPattern(64, 64);
        
        if (!pattern1.empty() && sampleData->texture1.Create(sampleData->service, 128, 128, pattern1.data())) {
            LOG_INFO("ImGuiTextureSample: Created texture1 (128x128)");
        } else {
            LOG_ERROR("ImGuiTextureSample: Failed to create texture1");
        }
        
        if (!pattern2.empty() && sampleData->texture2.Create(sampleData->service, 64, 64, pattern2.data())) {
            LOG_INFO("ImGuiTextureSample: Created texture2 (64x64)");
        } else {
            LOG_ERROR("ImGuiTextureSample: Failed to create texture2");
        }
        
        sampleData->lastDeviceGeneration = sampleData->service->GetDeviceGeneration();
        sampleData->texturesCreated = true;
    }

    void OnRender(void* data) {
        auto* sampleData = static_cast<TextureSampleData*>(data);
        
        ImGui::Begin("Texture Management Sample");
        
        if (!sampleData->service) {
            ImGui::TextUnformatted("Error: ImGui service not available");
            ImGui::End();
            return;
        }

        const uint32_t currentGen = sampleData->service->GetDeviceGeneration();
        const bool deviceReady = sampleData->service->IsDeviceReady();
        
        ImGui::Text("Device Generation: %u", currentGen);
        ImGui::Text("Device Ready: %s", deviceReady ? "Yes" : "No");
        
        // Detect device generation change
        if (sampleData->texturesCreated && currentGen != sampleData->lastDeviceGeneration) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), 
                "Warning: Device generation changed! Textures invalidated.");
            LOG_WARN("ImGuiTextureSample: Device generation changed ({} -> {})", 
                sampleData->lastDeviceGeneration, currentGen);
            sampleData->lastDeviceGeneration = currentGen;
            
            // Textures are automatically invalidated by the RAII wrapper
            // Need to recreate them
            sampleData->texture1.Release();
            sampleData->texture2.Release();
            sampleData->texturesCreated = false;
        }
        
        ImGui::Separator();
        
        // Display texture 1
        ImGui::Text("Texture 1 (128x128):");
        ImGui::Text("Valid: %s", sampleData->texture1.IsValid() ? "Yes" : "No");
        
        void* texId1 = sampleData->texture1.GetID();
        if (texId1) {
            ImGui::Image(texId1, ImVec2(128, 128));
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Texture not available");
        }
        
        ImGui::Separator();
        
        // Display texture 2
        ImGui::Text("Texture 2 (64x64):");
        ImGui::Text("Valid: %s", sampleData->texture2.IsValid() ? "Yes" : "No");
        
        void* texId2 = sampleData->texture2.GetID();
        if (texId2) {
            ImGui::Image(texId2, ImVec2(64, 64));
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Texture not available");
        }
        
        ImGui::Separator();
        
        // Manual recreation button
        if (!sampleData->texturesCreated) {
            if (ImGui::Button("Recreate Textures")) {
                OnInit(data);
            }
        }
        
        ImGui::End();
    }

    void OnShutdown(void* data) {
        auto* sampleData = static_cast<TextureSampleData*>(data);
        LOG_INFO("ImGuiTextureSample: OnShutdown called");
        
        // RAII wrapper automatically releases textures in destructor
        sampleData->texture1.Release();
        sampleData->texture2.Release();
        
        if (sampleData->service) {
            sampleData->service->Release();
            sampleData->service = nullptr;
        }
        
        delete sampleData;
    }
}

class ImGuiTextureSampleDirector : public cRZCOMDllDirector
{
public:
    bool OnStart(cIGZCOM* pCOM) override {
        Logger::Initialize("SC4ImGuiTextureSample", "");
        LOG_INFO("ImGuiTextureSample: plugin starting");
        
        cIGZFrameWork* fw = RZGetFrameWork();
        if (!fw) {
            LOG_ERROR("ImGuiTextureSample: framework not available");
            return false;
        }

        if (!fw->AddHook(this)) {
            LOG_ERROR("ImGuiTextureSample: failed to add framework hook");
            return false;
        }

        return true;
    }

    bool PreFrameWorkInit() override {
        cIGZFrameWork* fw = RZGetFrameWork();
        if (!fw) {
            return false;
        }

        cIGZImGuiService* service = nullptr;
        if (!fw->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                  reinterpret_cast<void**>(&service))) {
            LOG_WARN("ImGuiTextureSample: ImGui service not available yet");
            return true;
        }

        auto* sampleData = new TextureSampleData();
        sampleData->service = service;  // Service is AddRef'd by GetSystemService

        ImGuiPanelDesc desc{};
        desc.id = 0xF00D0001;  // Unique panel ID
        desc.order = 200;
        desc.visible = true;
        desc.on_init = &OnInit;
        desc.on_render = &OnRender;
        desc.on_shutdown = &OnShutdown;
        desc.data = sampleData;

        if (!service->RegisterPanel(desc)) {
            LOG_ERROR("ImGuiTextureSample: failed to register panel");
            delete sampleData;
            service->Release();
            return false;
        }

        LOG_INFO("ImGuiTextureSample: registered texture sample panel");
        return true;
    }

    uint32_t GetDirectorID() const override {
        return 0xF00D0001;
    }
};

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static ImGuiTextureSampleDirector director;
    return &director;
}
