/**
 * @file main.cpp
 * @brief Prism Render Engine
 *
 * @copyright Copyright (c) 2025 src-lua
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Prism.hpp"
#include "Prism/scene/scene_parser.hpp"
#include "Prism/vulkan/vulkan_context.hpp"
#include "Prism/vulkan/vulkan_renderer.hpp"
#include "Prism/vulkan/vulkan_scene_data.hpp"
#include <iostream>

void RenderScene(std::string shaderPath, std::string filename) {
    Prism::VulkanContext vulkanContext;

    // 2. Parse a cena do ficheiro YAML para um objeto Scene
    Prism::Style::logInfo("Parsing scene from YAML file...");
    Prism::Scene scene = Prism::SceneParser("./data/input/scene.yml").parse();
    Prism::Style::logDone("Scene parsed successfully.");

    // 3. Converta o objeto Scene para o formato da GPU
    Prism::Style::logInfo("Converting scene data for GPU...");
    Prism::SceneDataGPU sceneData(scene);
    Prism::Style::logDone("Scene data converted.");

    // 4. Crie o renderer passando os dados da cena convertidos
    const int imageWidth = scene.getCamera().pixel_width;
    const int imageHeight = scene.getCamera().pixel_height;
    Prism::VulkanRenderer renderer(vulkanContext, sceneData, imageWidth, imageHeight, shaderPath);

    // 5. Renderize e salve a imagem
    auto start = std::chrono::high_resolution_clock::now();
    renderer.render();
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;
    Prism::Style::logInfo("GPU Render Time: " + std::to_string(elapsed.count()) +
                          " ms"); // Corrigido para ms

    renderer.saveImage("output_gpu_" + filename + ".ppm");
}

int main() {
    try {
        Prism::SceneParser("./data/input/scene.yml").parse().render();

        std::clog << "\n\n\n";
        RenderScene("./shaders/pathtracer.spv", "pathtracer");
        std::clog << "\n\n\n";
        RenderScene("./shaders/raytracer.spv", "raytracer");
        std::clog << "\n\n\n";
        RenderScene("./shaders/raytracer_recursive.spv", "raytracer_recursive");

    } catch (const std::exception& e) {
        Prism::Style::logError(e.what());
        return 1;
    }

    return 0;
}