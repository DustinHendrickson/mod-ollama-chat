# Download/Update Script for nlohmann/json
# Run this if you need to update the bundled nlohmann/json header
#
# Windows PowerShell:
# Invoke-WebRequest -Uri "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" -OutFile "json.hpp"
#
# Linux/macOS:
# curl -L -o json.hpp https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
#
# wget:
# wget https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
#
# This file is automatically included by the CMake build system.