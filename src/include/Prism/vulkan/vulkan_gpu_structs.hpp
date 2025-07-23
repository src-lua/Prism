#ifndef PRISM_VULKAN_GPU_STRUCTS_HPP_
#define PRISM_VULKAN_GPU_STRUCTS_HPP_

#include <cstdint>

namespace Prism {

struct GPUCameraData {
    alignas(16) float position[3];
    alignas(16) float view_u[3];
    alignas(16) float view_v[3];
    alignas(16) float pixel_00_loc[3];
    alignas(16) float ambient_color[3];
    uint32_t image_width;
    uint32_t image_height;

    uint32_t samples_per_pixel;
};

struct GPUMaterial {
    float color[3];
    float refraction_index;

    float ambient[3];
    float transparency; 

    float specular[3];
    float shininess; 
    
    alignas(16) float emission[3];
};

struct GPUObject {
    alignas(4) int type; // 0 = Esfera, 1 = Plano, etc.
    alignas(4) int material_index; // Índice para um array de materiais
    alignas(8) float padding[2];

    float center[3];
    float radius;

    alignas(16) float point_on_plane[3];
    alignas(16) float normal[3];

    alignas(16) float transform[16];
    alignas(16) float inverse_transform[16];
    alignas(16) float inverse_transpose_transform[16];
};

struct GPULight {
    alignas(16) float position[3];
    alignas(16) float color[3];
};

} // namespace Prism

#endif // PRISM_VULKAN_GPU_STRUCTS_HPP_
