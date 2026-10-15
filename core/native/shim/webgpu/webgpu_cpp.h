// Native mode (gc3ds): Aurora's GX state header (lib/gx/gx.hpp) names WebGPU types for its own renderer. The 3DS
// renderer is gc3ds's (GX FIFO -> PICA200), so these only have to exist: empty handles and the few plain structs and
// enums the headers use. Nothing here draws.
#pragma once
#include <cstdint>
#include <string_view>
namespace wgpu {
struct Handle {
    explicit operator bool() const { return false; }
};
struct Instance : Handle {};
struct Device : Handle {};
struct Queue : Handle {};
struct Surface : Handle {};
struct Buffer : Handle {};
struct Texture : Handle {};
struct TextureView : Handle {};
struct Sampler : Handle {};
struct ShaderModule : Handle {};
struct RenderPipeline : Handle {};
struct BindGroup : Handle {};
struct CommandEncoder : Handle {};
struct RenderPassEncoder : Handle {};
enum class TextureFormat : uint32_t { Undefined, R8Unorm, RG8Unorm, RGBA8Unorm, BGRA8Unorm, RGB10A2Unorm, Depth32Float };
enum class BackendType : uint32_t { Undefined, Null };
enum class LoadOp : uint32_t { Undefined, Load, Clear };
enum class StoreOp : uint32_t { Undefined, Store, Discard };
struct Color {
    double r = 0, g = 0, b = 0, a = 0;
};
struct Extent3D {
    uint32_t width = 0;
    uint32_t height = 1;
    uint32_t depthOrArrayLayers = 1;
};
struct StringView {
    const char* data = nullptr;
    size_t length = 0;
    operator std::string_view() const { return data ? std::string_view(data, length) : std::string_view(); }
};
struct TexelCopyBufferLayout {
    uint64_t offset = 0;
    uint32_t bytesPerRow = 0;
    uint32_t rowsPerImage = 0;
};
struct TexelCopyTextureInfo {
    Texture texture;
    uint32_t mipLevel = 0;
};
struct SamplerDescriptor {};
struct SurfaceConfiguration {};
struct AdapterInfo {};
struct VertexBufferLayout {};
}  // namespace wgpu
