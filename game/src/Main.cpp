#include "GameApplication.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
    try {
        GameApplication application;
        application.Run();
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}