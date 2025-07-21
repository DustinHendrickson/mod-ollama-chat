# Find required packages
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

if(TARGET modules)
    if(WIN32)
        # Windows platform
        target_link_libraries(modules PRIVATE
            CURL::libcurl
            nlohmann_json::nlohmann_json
        )
    else()
        # Linux/Unix platform
        target_link_libraries(modules PRIVATE curl)
        target_include_directories(modules PRIVATE /usr/local/include /usr/local/include/nlohmann)
    endif()
endif()