# Ensure the module is correctly registered before linking
if(TARGET modules)
    # Include nlohmann/json library (bundled with module)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/deps/nlohmann/json.hpp")
        target_include_directories(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/deps)
        message(STATUS "[mod-ollama-chat] Using bundled nlohmann/json")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/deps/nlohmann")
        target_include_directories(modules PRIVATE ${CMAKE_SOURCE_DIR}/deps/nlohmann)
        message(STATUS "[mod-ollama-chat] Using AzerothCore deps nlohmann/json")
    else()
        find_package(nlohmann_json CONFIG QUIET)
        if(nlohmann_json_FOUND)
            target_link_libraries(modules PRIVATE nlohmann_json::nlohmann_json)
            message(STATUS "[mod-ollama-chat] Using system nlohmann/json")
        else()
            message(FATAL_ERROR "[mod-ollama-chat] nlohmann/json not found. The module includes a bundled version in deps/nlohmann/json.hpp - please ensure this file exists")
        endif()
    endif()

    # Include fmt library
    if(TARGET fmt)
        # Use main AzerothCore fmt
        target_link_libraries(modules PRIVATE fmt)
        message(STATUS "[mod-ollama-chat] Using AzerothCore fmt library")
    else()
        # Try to find fmt via find_package (vcpkg/system packages)
        find_package(fmt CONFIG QUIET)
        if(fmt_FOUND)
            target_link_libraries(modules PRIVATE fmt::fmt)
            message(STATUS "[mod-ollama-chat] Using system fmt library")
        else()
            message(FATAL_ERROR "[mod-ollama-chat] fmt library not found. Please install it:\n"
                              "  Windows (vcpkg): vcpkg install fmt\n"
                              "  Ubuntu/Debian: sudo apt install libfmt-dev\n"
                              "  CentOS/RHEL: sudo yum install fmt-devel\n"
                              "  macOS: brew install fmt\n"
                              "  Or build AzerothCore with full deps")
        endif()
    endif()
    
    # Include cpp-httplib library (header-only)
    target_include_directories(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
    
    # Enable SSL/TLS support for HTTPS connections
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND OR OPENSSL_FOUND)
        target_compile_definitions(modules PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
        target_link_libraries(modules PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        message(STATUS "[mod-ollama-chat] OpenSSL found - HTTPS support enabled")
    else()
        message(WARNING "[mod-ollama-chat] OpenSSL not found - only HTTP support available")
        message(STATUS "[mod-ollama-chat] To enable HTTPS: install OpenSSL development libraries")
        if(WIN32)
            message(STATUS "[mod-ollama-chat] Windows: Install vcpkg and run 'vcpkg install openssl'")
        else()
            message(STATUS "[mod-ollama-chat] Linux: apt install libssl-dev (Ubuntu/Debian) or yum install openssl-devel (RHEL/CentOS)")
        endif()
    endif()
    
    # Platform-specific threading and networking libraries
    if(WIN32)
        # Windows requires winsock for networking and additional SSL libraries
        target_link_libraries(modules PRIVATE ws2_32 crypt32)
    else()
        # Linux/macOS requires pthread
        target_link_libraries(modules PRIVATE pthread)
    endif()
endif()