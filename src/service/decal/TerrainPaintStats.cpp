#include "TerrainPaintStats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <imgui.h>

#include "utils/Logger.h"

namespace TerrainPaintStats
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr auto kPassCount = static_cast<size_t>(Pass::Count);
        constexpr std::array<const char*, kPassCount> kPassNames{
            "Over lots", "Under lots", "Over shadows", "Over sidewalks", "Shadow recovery", "Shadow recovery capture"};
        constexpr size_t kTopPaints = 5;

        struct PaintTotals
        {
            uint32_t textureCount = 0;
            double vertices = 0.0;
            double ms = 0.0;
        };

        // Per-frame averages are these sums divided by the frame count.
        struct Edits
        {
            uint32_t count = 0;
            double ms = 0.0;
            double meshMs = 0.0;
        };

        struct Window
        {
            std::array<Counters, kPassCount> passes{};
            std::unordered_map<uint32_t, PaintTotals> paints;
            Edits edits;
            uint32_t frames = 0;
            Clock::time_point start = Clock::now();
        };

        struct Published
        {
            std::array<Counters, kPassCount> passes{};
            Counters total{};
            std::vector<std::pair<uint32_t, PaintTotals>> top;
            Edits edits;
            uint32_t frames = 0;
            double seconds = 0.0;
        };

        Window window;
        Published published;
        // The slowest edit since the last log line, and the one shown until then.
        Edit slowestEdit;
        Edit shownSlowestEdit;
        Clock::time_point lastLog = Clock::now();

        void Accumulate(Counters& into, const Counters& c) noexcept
        {
            into.overlays += c.overlays;
            into.rows += c.rows;
            into.cells += c.cells;
            into.vertices += c.vertices;
            into.drawCalls += c.drawCalls;
            into.deferredRows += c.deferredRows;
            into.buildMs += c.buildMs;
            into.liftMs += c.liftMs;
            into.submitMs += c.submitMs;
        }

        double PerFrame(const uint64_t value) { return published.frames ? double(value) / published.frames : 0.0; }
        double PerFrame(const double value) { return published.frames ? value / published.frames : 0.0; }

        void Publish(const Clock::time_point now)
        {
            published.passes = window.passes;
            published.total = {};
            for (const Counters& pass : window.passes) {
                Accumulate(published.total, pass);
            }
            published.top.assign(window.paints.begin(), window.paints.end());
            const size_t top = std::min(kTopPaints, published.top.size());
            std::partial_sort(published.top.begin(), published.top.begin() + static_cast<std::ptrdiff_t>(top),
                              published.top.end(),
                              [](const auto& a, const auto& b) { return a.second.vertices > b.second.vertices; });
            published.top.resize(top);
            published.edits = window.edits;
            published.frames = window.frames;
            published.seconds = std::chrono::duration<double>(now - window.start).count();
            window = Window{};
            window.start = now;
        }

        std::string Segment(const char* name, const Counters& c)
        {
            return std::format("{}: overlays {:.1f}, rows {:.1f}, deferred {:.1f}, cells {:.0f}, vertices {:.0f}, "
                               "draws {:.1f}, build {:.2f} ms, lift {:.2f} ms, submit {:.2f} ms",
                               name, PerFrame(c.overlays), PerFrame(c.rows), PerFrame(c.deferredRows), PerFrame(c.cells),
                               PerFrame(c.vertices), PerFrame(c.drawCalls), PerFrame(c.buildMs),
                               PerFrame(c.liftMs), PerFrame(c.submitMs));
        }

        std::string Describe(const Edit& e)
        {
            if (e.ms <= 0.0) {
                return "none";
            }
            return std::format("{} paint {}: {:.2f} ms, mesh {:.2f} ms, rebuilt {}{}", e.kind, e.paintId, e.ms,
                               e.meshMs, e.newMesh ? "mesh" : "no mesh", e.newOverlays ? " and overlays" : "");
        }

        double PerSecond(const double value) { return published.seconds > 0.0 ? value / published.seconds : 0.0; }

        void Log()
        {
            std::string line = std::format("Terrain paint stats per frame, {} frames in {:.1f} s ({:.0f} fps) | ",
                                           published.frames, published.seconds,
                                           published.frames / published.seconds);
            line += Segment("Total", published.total);
            for (size_t i = 0; i < kPassCount; ++i) {
                if (published.passes[i].overlays > 0) {
                    line += " | " + Segment(kPassNames[i], published.passes[i]);
                }
            }
            for (const auto& [id, paint] : published.top) {
                line += std::format(" | paint {} ({} textures): {:.0f} vertices, {:.2f} ms", id, paint.textureCount,
                                    PerFrame(paint.vertices), PerFrame(paint.ms));
            }
            line += std::format(" | edits per second: {:.1f} calls, {:.2f} ms, mesh {:.2f} ms | slowest edit in 10 s: {}",
                                PerSecond(published.edits.count), PerSecond(published.edits.ms),
                                PerSecond(published.edits.meshMs), Describe(shownSlowestEdit));
            LOG_INFO("{}", line);
        }

        void Row(const char* name, const Counters& c)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", PerFrame(c.overlays));
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", PerFrame(c.rows));
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", PerFrame(c.deferredRows));
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", PerFrame(c.cells));
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", PerFrame(c.vertices));
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", PerFrame(c.drawCalls));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", PerFrame(c.buildMs));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", PerFrame(c.liftMs));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", PerFrame(c.submitMs));
        }
    }

    double NowMs() noexcept
    {
        return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
    }

    void Add(const Pass pass, const uint32_t paintId, const uint32_t textureCount, const Counters& counters)
    {
        Accumulate(window.passes[static_cast<size_t>(pass)], counters);
        PaintTotals& paint = window.paints[paintId];
        paint.textureCount = textureCount;
        paint.vertices += static_cast<double>(counters.vertices);
        paint.ms += counters.buildMs + counters.liftMs + counters.submitMs;
    }

    void AddEdit(const Edit& edit)
    {
        ++window.edits.count;
        window.edits.ms += edit.ms;
        window.edits.meshMs += edit.meshMs;
        if (edit.ms > slowestEdit.ms) {
            slowestEdit = edit;
        }
        if (edit.ms > shownSlowestEdit.ms) {
            shownSlowestEdit = edit;
        }
    }

    void EndFrame()
    {
        ++window.frames;
        const Clock::time_point now = Clock::now();
        if (now - window.start < std::chrono::seconds(1)) {
            return;
        }
        Publish(now);
        if (now - lastLog >= std::chrono::seconds(10)) {
            lastLog = now;
            shownSlowestEdit = slowestEdit;
            Log();
            slowestEdit = Edit{};
        }
    }

    void RenderPanel()
    {
        ImGui::SetNextWindowSize(ImVec2(760.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Terrain paint stats")) {
            ImGui::End();
            return;
        }
        ImGui::Text("Per frame, average of the last %.1f s (%u frames, %.0f fps).", published.seconds,
                    published.frames, published.seconds > 0.0 ? published.frames / published.seconds : 0.0);
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("passes", 10, kFlags)) {
            for (const char* header : {"Pass", "Overlays", "Rows built", "Rows put off", "Cells", "Vertices", "Draw calls",
                                       "Build ms", "Lift ms", "Submit ms"}) {
                ImGui::TableSetupColumn(header);
            }
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < kPassCount; ++i) {
                Row(kPassNames[i], published.passes[i]);
            }
            Row("Total", published.total);
            ImGui::EndTable();
        }

        ImGui::TextUnformatted("Cells are cut (soft edge, wear) when first drawn: that is in Build ms.");

        ImGui::Spacing();
        ImGui::Text("Paint edits (CreatePaint, SetPaintShape, SetPaintStyle) per second: %.1f calls, %.2f ms, "
                    "of which making meshes %.2f ms.",
                    PerSecond(published.edits.count), PerSecond(published.edits.ms), PerSecond(published.edits.meshMs));
        ImGui::Text("Slowest edit in the last 10 s: %s", Describe(shownSlowestEdit).c_str());

        ImGui::Spacing();
        ImGui::TextUnformatted("Paints with the most vertices, all passes:");
        if (ImGui::BeginTable("paints", 4, kFlags)) {
            for (const char* header : {"Paint", "Textures", "Vertices", "ms"}) {
                ImGui::TableSetupColumn(header);
            }
            ImGui::TableHeadersRow();
            for (const auto& [id, paint] : published.top) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", id);
                ImGui::TableNextColumn();
                ImGui::Text("%u", paint.textureCount);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", PerFrame(paint.vertices));
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", PerFrame(paint.ms));
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }
}
