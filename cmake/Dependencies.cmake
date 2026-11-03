# cmake/Dependencies.cmake

find_package(Vulkan 1.4 REQUIRED)

find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

# stb_image (header-only PNG/JPEG decoding, used by ImageImporter). vcpkg's stb port ships a
# FindStb module that sets Stb_INCLUDE_DIR; fall back to a plain header search for system installs
# (e.g. Debian's libstb-dev, which nests the headers under include/stb).
find_package(Stb QUIET)
if(NOT Stb_INCLUDE_DIR)
    find_path(Stb_INCLUDE_DIR NAMES stb_image.h PATH_SUFFIXES stb)
endif()
if(NOT Stb_INCLUDE_DIR)
    message(FATAL_ERROR "stb_image.h not found. Install the 'stb' vcpkg port (listed in vcpkg.json) or set "
                        "Stb_INCLUDE_DIR to the directory containing it.")
endif()