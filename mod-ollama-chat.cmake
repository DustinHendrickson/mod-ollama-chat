# Register the module with AzerothCore if the macro is defined
if(COMMAND add_ac_module)
    add_ac_module(mod-ollama-chat "${CMAKE_CURRENT_LIST_DIR}")
endif()

# Dependencies: CURL and nlohmann_json from vcpkg
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

# Link the module if the 'modules' target exists
if(TARGET modules)
    # For windows use CURL::libcurl which is properly defined by vcpkg’s toolchain
    target_link_libraries(modules PRIVATE CURL::libcurl)

    # Link JSON
    target_link_libraries(modules PRIVATE nlohmann_json::nlohmann_json)
endif()
