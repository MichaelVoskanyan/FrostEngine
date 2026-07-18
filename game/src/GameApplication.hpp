#pragma once
#include <Frost/Application.hpp>

class GameApplication final : public Frost::Application {
public:
    GameApplication();
    ~GameApplication() override = default;
};