# cmake/Dependencies.cmake

find_package(Vulkan 1.4 REQUIRED)

find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

# Dear ImGui with its GLFW and Vulkan backends (vcpkg features glfw-binding/vulkan-binding; see
# vcpkg.json). The engine hosts it (ImGuiLayer); the editor builds its UI with it.
find_package(imgui CONFIG REQUIRED)

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

# miniaudio (audio playback, used by src/Audio/AudioEngine.cpp only). A miniaudio CMake package
# with a compiled library is linked if there is one (recent vcpkg ports may install one); otherwise
# only the single header is needed, and the engine compiles miniaudio's implementation into
# AudioEngine.cpp itself (FROST_MINIAUDIO_IMPLEMENTATION). Either way no engine header includes it.
# Threads first: miniaudio needs them either way, and a package's exported targets may name
# Threads::Threads without finding it themselves.
find_package(Threads REQUIRED)
find_package(miniaudio CONFIG QUIET)
if(TARGET miniaudio::miniaudio)
    set(FROST_MINIAUDIO_TARGET miniaudio::miniaudio)
elseif(TARGET miniaudio)
    set(FROST_MINIAUDIO_TARGET miniaudio)
else()
    find_path(Miniaudio_INCLUDE_DIR NAMES miniaudio.h PATH_SUFFIXES miniaudio)
    if(NOT Miniaudio_INCLUDE_DIR)
        message(FATAL_ERROR "miniaudio.h not found. Install the 'miniaudio' vcpkg port (listed in vcpkg.json) or "
                            "set Miniaudio_INCLUDE_DIR to the directory containing it.")
    endif()
    # Its implementation also needs, on Linux, dlopen (it loads the system's audio libraries at run
    # time) and libm (see engine/CMakeLists.txt).
endif()