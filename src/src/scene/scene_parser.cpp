/**
 * @file scene_parser.cpp
 * @brief Prism Render Engine
 *
 * @copyright Copyright (c) 2025 src-lua
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifdef PRISM_BUILD_SCENE

#include "Prism/scene/scene_parser.hpp"

#include "Prism/core/color.hpp"
#include "Prism/core/material.hpp"
#include "Prism/core/matrix.hpp"
#include "Prism/core/point.hpp"
#include "Prism/core/style.hpp"
#include "Prism/core/texture.hpp"
#include "Prism/core/utils.hpp"
#include "Prism/core/vector.hpp"
#include "Prism/objects/mesh.hpp"
#include "Prism/objects/plane.hpp"
#include "Prism/objects/sphere.hpp"
#include "Prism/objects/triangle.hpp"
#include "Prism/scene/camera.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846 // Define M_PI if not already defined
#endif

namespace Prism {

// --- Parsing Helper Functions ---

// Converts a YAML node [x, y, z] to a Point3
Point3 parsePoint(const YAML::Node& node) {
    if (!node.IsSequence() || node.size() != 3) {
        throw std::runtime_error("Parsing error: Malformed 3D point.");
    }
    return Point3(node[0].as<double>(), node[1].as<double>(), node[2].as<double>());
}

// Converts a YAML node [x, y, z] to a Vector3
Vector3 parseVector(const YAML::Node& node) {
    if (!node.IsSequence() || node.size() != 3) {
        throw std::runtime_error("Parsing error: Malformed 3D vector.");
    }
    return Vector3(node[0].as<double>(), node[1].as<double>(), node[2].as<double>());
}

Color parseColor(const YAML::Node& node) {
    if (!node.IsSequence() || node.size() != 3) {
        throw std::runtime_error("Parsing error: Malformed color.");
    }
    return Color(node[0].as<double>(), node[1].as<double>(), node[2].as<double>());
}

// Converts a YAML node with material properties to a Material
std::shared_ptr<Material> parseMaterial(const YAML::Node& node,
                                        const std::filesystem::path& scene_dir) {
    auto mat = std::make_shared<Material>();

    if (node["texture"]) {
        std::string texture_type = node["texture"]["type"].as<std::string>();
        if (texture_type == "solid") {
            Color color = parseColor(node["texture"]["color"]);
            mat->texture = std::make_shared<SolidColor>(color);
        }
        else if (texture_type == "checker") {
            Color color1 = parseColor(node["texture"]["color1"]);
            Color color2 = parseColor(node["texture"]["color2"]);
            double scale = node["texture"]["scale"].as<double>(1.0);
            mat->texture = std::make_shared<CheckerTexture>(scale, color1, color2);
        } 
        else if(texture_type == "stripes") {
            Color color1 = parseColor(node["texture"]["color1"]);
            Color color2 = parseColor(node["texture"]["color2"]);
            double scale = node["texture"]["scale"].as<double>(1.0);
            mat->texture = std::make_shared<StripesTexture>(scale, color1, color2);
        } 
        else if (texture_type == "sinewave") {
            Color color1 = parseColor(node["texture"]["color1"]);
            Color color2 = parseColor(node["texture"]["color2"]);
            double scale = node["texture"]["scale"].as<double>(1.0);
            mat->texture = std::make_shared<SineWaveTexture>(scale, color1, color2);
        }
        else if (texture_type == "wavy_stripes") {
            Color color1 = parseColor(node["texture"]["color1"]);
            Color color2 = parseColor(node["texture"]["color2"]);
            double stripe_density = node["texture"]["stripe_density"].as<double>(1.0);
            double wave_frequency = node["texture"]["wave_frequency"].as<double>(1.0);
            double wave_amplitude = node["texture"]["wave_amplitude"].as<double>(0.5);
            mat->texture = std::make_shared<WavyStripesTexture>(stripe_density, wave_frequency, wave_amplitude, color1, color2);
        }
        else if (texture_type == "image" || texture_type == "img") {
            std::string path_str = node["texture"]["path"].as<std::string>();
            double scale = node["texture"]["scale"].as<double>(1.0);
            std::filesystem::path full_path = scene_dir / path_str;
            mat->texture = std::make_shared<ImageTexture>(full_path.string(), scale);
        }

        else {
            Style::logWarning("Unknown texture type: " + texture_type +
                              ". Defaulting to solid color.");
            mat->texture = std::make_shared<SolidColor>(1.0, 1.0, 1.0);
        }
    } else if (node["color"] && node["color"].IsSequence() && node["color"].size() == 3) {
        mat->texture = std::make_shared<SolidColor>(parseColor(node["color"]));
    } else if (node["color"]) {
        Vector3 v = parseVector(node["color"]);
        mat->texture = std::make_shared<SolidColor>(v.x, v.y, v.z);
    }

    if (node["ka"])
        mat->ka = parseColor(node["ka"]);
    if (node["ks"])
        mat->ks = parseColor(node["ks"]);
    if (node["ke"])
        mat->ke = parseColor(node["ke"]);
    if (node["ns"])
        mat->ns = node["ns"].as<double>();
    if (node["ni"])
        mat->ni = node["ni"].as<double>();
    if (node["d"])
        mat->d = node["d"].as<double>();
    return mat;
}

// Converts a YAML list of transformations into a single transformation matrix
Matrix parseTransformations(const YAML::Node& node) {
    Matrix final_transform = Matrix::identity(4);
    if (!node)
        return final_transform;

    // Transformations are applied in the reverse order of the list (standard in computer graphics)
    for (int i = node.size() - 1; i >= 0; --i) {
        const auto& transform_node = node[i];
        std::string type = transform_node["type"].as<std::string>();
        Matrix current_transform = Matrix::identity(4);

        if (type == "translation") {
            Vector3 v = parseVector(transform_node["vector"]);
            current_transform = Matrix::translation(v.x, v.y, v.z);
        } else if (type == "rotation") {
            double angle_deg = transform_node["angle"].as<double>();
            double angle_rad = angle_deg * (M_PI / 180.0); // Convert to radians
            current_transform = Matrix::rotation(angle_rad, parseVector(transform_node["axis"]));
        } else if (type == "scaling") {
            Vector3 v = parseVector(transform_node["factors"]);
            current_transform = Matrix::scaling(v.x, v.y, v.z);
        } else {
            Style::logWarning("Unknown transformation type: " + type +
                              ". Skipping this transformation.");
            continue; // Skip unknown transformation types
        }
        final_transform = final_transform * current_transform;
    }
    return final_transform;
}

// --- SceneParser Class Implementation ---

SceneParser::SceneParser(const std::string& sceneFilePath) : filePath(sceneFilePath) {
}

Scene SceneParser::parse(ACCELERATION acceleration) const {
    YAML::Node root;
    try {
        root = YAML::LoadFile(filePath);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Error loading YAML file: " + std::string(e.what()));
    }

    Style::logInfo("Parsing scene from file: " + Style::CYAN + filePath);

    // Parse the Camera
    if (!root["camera"]) {
        throw std::runtime_error("'camera' node not found in the scene file.");
    }
    const auto& cam_node = root["camera"];
    Camera camera(parsePoint(cam_node["lookfrom"]), parsePoint(cam_node["lookat"]),
                  parseVector(cam_node["vup"]), cam_node["screen_distance"].as<double>(),
                  cam_node["viewport_height"].as<double>(), cam_node["viewport_width"].as<double>(),
                  cam_node["image_height"].as<int>(), cam_node["image_width"].as<int>());

    Color ambient_light(0.1, 0.1, 0.1); // Valor padrão
    if (root["ambient_light"]) {
        Vector3 v = parseVector(root["ambient_light"]);
        ambient_light = Color(v.x, v.y, v.z);
    } else {
        Style::logWarning("Ambient light not defined. Using default (0.1, 0.1, 0.1).");
    }

    std::vector<std::unique_ptr<Object>> objects;
    std::vector<std::unique_ptr<Light>> lights;

    // Parse Material Definitions (for reuse)
    std::map<std::string, std::shared_ptr<Material>> materials;
    if (root["definitions"] && root["definitions"]["materials"]) {
        for (const auto& mat_node : root["definitions"]["materials"]) {
            std::string name = mat_node.first.as<std::string>();
            materials[name] =
                parseMaterial(mat_node.second, std::filesystem::path(filePath).parent_path());
        }
    }

    if (root["lights"] && root["lights"].IsSequence()) {
        for (const auto& light_node : root["lights"]) {
            std::string name = "Unnamed Light";
            if (light_node["name"]) {
                name = light_node["name"].as<std::string>();
            }

            std::string type = "point";
            if (light_node["type"]) {
                type = light_node["type"].as<std::string>();
            } else {
                Style::logWarning("Light type not specified for '" + name +
                                  "'. Defaulting to 'point'.");
            }

            Color color = parseColor(light_node["color"]);

            if (type == "point") {
                Point3 pos = parsePoint(light_node["position"]);
                lights.push_back(std::make_unique<PointLight>(pos, color));
            }

            else if (type == "quad") {
                Point3 corner = parsePoint(light_node["corner"]);
                Vector3 u_vec = parseVector(light_node["u_vec"]);
                Vector3 v_vec = parseVector(light_node["v_vec"]);

                lights.push_back(std::make_unique<QuadLight>(corner, u_vec, v_vec, color));
            }

            else if (type == "spherical") {
                Point3 center = parsePoint(light_node["center"]);
                double radius = light_node["radius"].as<double>();
                lights.push_back(std::make_unique<SphericalLight>(center, radius, color));
            }
        }
    } else {
        Style::logWarning("'lights' node not found or is not a list. No lights will be added.");
    }

    // Parse Objects
    if (!root["objects"] || !root["objects"].IsSequence()) {
        throw std::runtime_error("'objects' node not found or is not a list.");
    }

    for (const auto& obj_node : root["objects"]) {
        std::string type = obj_node["type"].as<std::string>();

        // Find the material (whether defined inline or by reference)
        std::shared_ptr<Material> material;
        if (obj_node["material"]) {

            if (obj_node["material"].IsMap()) {
                material =
                parseMaterial(obj_node["material"], std::filesystem::path(filePath).parent_path());
            } else if (obj_node["material"].IsScalar()) {
                std::string mat_name = obj_node["material"].as<std::string>();
                if (materials.count(mat_name)) {
                    material = materials.at(mat_name);
                } else {
                    throw std::runtime_error("Referenced material not found: " + mat_name);
                }
            }
        } else {
            material = std::make_shared<Material>(); // Default material
        }

        std::unique_ptr<Object> object;

        if (type == "sphere") {
            object = std::make_unique<Sphere>(parsePoint(obj_node["center"]),
                                              obj_node["radius"].as<double>(), material);
        } else if (type == "plane") {
            object = std::make_unique<Plane>(parsePoint(obj_node["point_on_plane"]),
                                             parseVector(obj_node["normal"]), material);
        } else if (type == "triangle") {
            object =
                std::make_unique<Triangle>(parsePoint(obj_node["p1"]), parsePoint(obj_node["p2"]),
                                           parsePoint(obj_node["p3"]), material);
        } else if (type == "mesh") {
            std::filesystem::path scene_dir = std::filesystem::path(filePath).parent_path();
            std::string mesh_path_str = obj_node["path"].as<std::string>();
            std::filesystem::path full_mesh_path = scene_dir / mesh_path_str;

            object = std::make_unique<Mesh>(full_mesh_path);
            // Overrides the .obj material with the one from the .yml, if specified
            if (obj_node["material"]) {
                auto mesh_ptr = static_cast<Mesh*>(object.get());
                mesh_ptr->setMaterial(material);
            }
        } else {
            Style::logWarning("Unknown object type: " + type + ". Skipping this object.");
            continue;
        }

        if (object) {
            object->setTransform(parseTransformations(obj_node["transform"]));
            objects.push_back(std::move(object));
        }
    }

    int faces = 0;
    for (const auto& obj : objects) {
        if (auto mesh = dynamic_cast<Mesh*>(obj.get())) {
            faces += mesh->getMesh().size();
        } else {
            // For other object types, we can assume 1 face per object
            faces += 1; // Sphere, Plane, Triangle
        }
    }


    Style::logDone("Scene parsing completed successfully.");
    Style::logInfo("Scene contains: " + Style::CYAN + std::to_string(objects.size()) +
                   " total objects (with "
                   + std::to_string(faces)
                   + " faces), "
                   + std::to_string(lights.size()) + " light sources.");

    Style::logSection();
    Style::logInfo("--- Scene Settings ---");
    Style::logInfo("Resolution: " + Style::CYAN +
                   std::to_string(cam_node["image_height"].as<int>()) + "x" +
                   std::to_string(cam_node["image_width"].as<int>()));
    Style::logInfo("Camera LookFrom: " + Style::CYAN + to_string(camera.pos));
    Style::logInfo("----------------------");
    Style::logSection();

    return Scene(camera, std::move(objects), std::move(lights), ambient_light, acceleration);
}

} // namespace Prism

#endif // PRISM_BUILD_SCENE