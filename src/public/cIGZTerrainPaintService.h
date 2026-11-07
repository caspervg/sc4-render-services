#pragma once

#include <cstddef>
#include <cstdint>

#include "cGZPersistResourceKey.h"
#include "cIGZUnknown.h"
#include "cS3DVector3.h"

// Terrain paint: an area of any shape, painted onto the ground with a texture or a colour.
//
// A paint is a shape plus a style. The shape is a set of polygon rings in the city's ground plane
// (x and z in metres). The style says what the area looks like: which textures, how big they are
// and how they are turned, a colour, an opacity, a soft edge, and where the paint sits among the
// other paints and decals. The service drapes the area over the terrain, follows later terrain
// edits, draws it under the game's shadows like a decal, and keeps it in the city save.
//
// The interface is implemented by the terrain decal service. Query it with
// GZIID_cIGZTerrainPaintService from the service you get for kTerrainDecalServiceID.
// Needs SC4 1.1.641 and EnableCustomTerrainDecalRenderer=true.

struct TerrainPaintId {
    uint32_t value = 0;
};

struct TerrainPaintPoint {
    float x = 0.0f;
    float z = 0.0f;
};

// One corner of a UV map triangle: a point on the ground and the texture coordinates there, in
// textures (u = 1 is one texture width).
struct TerrainPaintUvVertex {
    float x = 0.0f;
    float z = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};

// The area is everything inside an odd number of rings (the even-odd rule), so a ring inside
// another ring cuts a hole. Rings close themselves; do not repeat the first point at the end.
//
// Without a UV map the textures lie flat on the ground, sized and turned by the style. A UV map
// lets them follow a path instead, such as a dirt track along its curve: triangles, three corners
// each, that say which texture coordinates each point of the area gets, linearly within each
// triangle. They should cover the area; parts of the area outside them are not drawn. The style's
// texture size, turn and offset do not apply then.
struct TerrainPaintShape {
    const TerrainPaintPoint* points = nullptr;
    // Number of points of each ring, in order. Rings with fewer than 3 points are ignored.
    const uint32_t* ringSizes = nullptr;
    uint32_t ringCount = 0;
    // Optional: three per triangle.
    const TerrainPaintUvVertex* uvMap = nullptr;
    uint32_t uvVertexCount = 0;
};

static constexpr uint32_t kTerrainPaintMaxTextures = 8;

struct TerrainPaintTexture {
    // A terrain decal texture (FSH, for example type 0x7AB50E44, group 0x0986135E).
    cGZPersistResourceKey key{};
    // Relative share of the 16 m terrain cells that get this texture. Ignored with one texture.
    float weight = 1.0f;
};

// Not stored in the city save. Use it for previews.
static constexpr uint32_t kTerrainPaintFlagTransient = 1u << 0;

struct TerrainPaintStyle {
    // One to kTerrainPaintMaxTextures textures. With more than one, every 16 m terrain cell of the
    // area picks one of them by weight, the same one every time for the same seed.
    // For a plain colour, use a white texture and set color.
    TerrainPaintTexture textures[kTerrainPaintMaxTextures]{};
    uint32_t textureCount = 0;
    uint32_t variationSeed = 0;

    // The texture repeats every textureSizeMeters metres, turned by textureRotationTurns
    // (1 = a full turn) and shifted by the offset. The defaults match the game's 16 m tiles.
    float textureSizeMeters = 16.0f;
    float textureRotationTurns = 0.0f;
    float textureOffsetX = 0.0f;
    float textureOffsetZ = 0.0f;

    cS3DVector3 color = cS3DVector3(1.0f, 1.0f, 1.0f);
    float opacity = 1.0f;
    // Width of the soft edge, inside the shape. The paint fades from full opacity at this
    // distance from the outline to nothing at the outline. 0 gives a hard edge.
    float featherMeters = 0.0f;

    // Paints and decals draw in ascending order, so a higher value lands on top. The game's own
    // decals count as 0; equal values keep the order the paints were made in.
    int32_t drawOrder = 0;
    // -1 uses the service default (INI: TerrainDecalCustomDefaultDepthOffset). See
    // TerrainDecalState::depthOffset.
    int32_t depthOffset = -1;
    bool enabled = true;
    uint32_t flags = 0;

    // Worn paint: patches missing from about this share of the paint (0 to 0.95), each about
    // wearPatchMeters across, placed by wearSeed. Their edges are hard, whatever the soft edge.
    float wear = 0.0f;
    float wearPatchMeters = 0.5f;
    uint32_t wearSeed = 0;
};

static constexpr size_t kTerrainPaintStyleSize = sizeof(TerrainPaintStyle);

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
class cIGZTerrainPaintService : public cIGZUnknown {
public:
    [[nodiscard]] virtual uint32_t GetStyleSize() const = 0;

    // Fails when no city is loaded, the shape has no area, or the style is invalid.
    virtual bool CreatePaint(const TerrainPaintShape& shape, const TerrainPaintStyle* style, uint32_t styleSize,
                             TerrainPaintId* outId) = 0;
    virtual bool RemovePaint(TerrainPaintId id) = 0;

    virtual bool SetPaintShape(TerrainPaintId id, const TerrainPaintShape& shape) = 0;
    virtual bool SetPaintStyle(TerrainPaintId id, const TerrainPaintStyle* style, uint32_t styleSize) = 0;
    virtual bool GetPaintStyle(TerrainPaintId id, TerrainPaintStyle* outStyle, uint32_t styleSize) const = 0;

    [[nodiscard]] virtual uint32_t GetPaintCount() const = 0;
};
