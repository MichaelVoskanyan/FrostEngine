#include "GameApplication.hpp"

GameApplication::GameApplication()
    : Frost::Application(Frost::EngineSpecification{.applicationName = "Cesium",
                                                    .windowWidth = 1280,
                                                    .windowHeight = 720,
                                                    .windowResizable = true}) {}