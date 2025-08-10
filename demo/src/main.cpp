/**
 * @file main.cpp
 * @brief Prism Render Engine
 *
 * @copyright Copyright (c) 2025 src-lua
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Prism.hpp"
#include <iostream>

int main() {
    try {
        Prism::Style::logInfo("FIRST TEST: Rendering with Octree acceleration.");
        
        Prism::SceneParser("./data/input/scene.yml").parse(Prism::ACCELERATION::OCTREE).render();
        
        Prism::Style::logInfo("SECOND TEST: Rendering without acceleration.");

        Prism::SceneParser("./data/input/scene.yml").parse(Prism::ACCELERATION::NONE).render();

    } catch (const std::exception& e) {
        Prism::Style::logError(e.what());
        return 1;
    }

    return 0;
}