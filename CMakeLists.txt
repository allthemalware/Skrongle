cmake_minimum_required(VERSION 3.16)
project(Skrongle CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Default to an optimized build so a bare `cmake -B build` does not ship a debug
# DSP core.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)   # std::thread (GUI fallback timer)

set(SW_SOURCES
    src/plugin.cpp
    src/gui/gui_core.cpp
)

if(WIN32)
    list(APPEND SW_SOURCES src/gui/platform_win32.cpp)
elseif(APPLE)
    enable_language(OBJCXX)
    list(APPEND SW_SOURCES src/gui/platform_macos.mm)
else()
    list(APPEND SW_SOURCES src/gui/platform_x11.cpp)
endif()

add_library(skrongle MODULE ${SW_SOURCES})
target_include_directories(skrongle PRIVATE clap/include)
target_link_libraries(skrongle PRIVATE Threads::Threads)
set_target_properties(skrongle PROPERTIES PREFIX "" OUTPUT_NAME "skrongle")

if(WIN32)
    # Linux/Windows: CLAP loads a flat file named <name>.clap.
    set_target_properties(skrongle PROPERTIES SUFFIX ".clap")
    target_link_libraries(skrongle PRIVATE user32 gdi32)
elseif(APPLE)
    # CLAP on macOS is a *bundle*, not a flat dylib (see clap/entry.h: the
    # plugin_path is "the bundle (macOS)"). Build the loadable module, then
    # assemble skrongle.clap/Contents/{MacOS,Info.plist,PkgInfo} around it.
    set_target_properties(skrongle PROPERTIES SUFFIX "")
    target_link_libraries(skrongle PRIVATE "-framework Cocoa")

    set(_plist "${CMAKE_BINARY_DIR}/Info.plist")
    file(WRITE "${_plist}"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>CFBundleExecutable</key><string>skrongle</string>
  <key>CFBundleIdentifier</key><string>com.openglitch.skrongle</string>
  <key>CFBundleName</key><string>Skrongle</string>
  <key>CFBundleDisplayName</key><string>Skrongle</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleShortVersionString</key><string>1.0.0</string>
  <key>CFBundleVersion</key><string>1.0.0</string>
  <key>CFBundleSignature</key><string>????</string>
  <key>LSMinimumSystemVersion</key><string>10.11</string>
</dict>
</plist>
")
    file(WRITE "${CMAKE_BINARY_DIR}/PkgInfo" "BNDL????")

    set(_bundle "${CMAKE_BINARY_DIR}/skrongle.clap")
    add_custom_command(TARGET skrongle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_bundle}/Contents/MacOS"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:skrongle>" "${_bundle}/Contents/MacOS/skrongle"
        COMMAND ${CMAKE_COMMAND} -E copy "${_plist}" "${_bundle}/Contents/Info.plist"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/PkgInfo" "${_bundle}/Contents/PkgInfo"
        COMMENT "Assembling skrongle.clap bundle")
else()
    set_target_properties(skrongle PROPERTIES SUFFIX ".clap")
    find_package(X11 REQUIRED)
    target_link_libraries(skrongle PRIVATE ${X11_LIBRARIES} m)
    target_include_directories(skrongle PRIVATE ${X11_INCLUDE_DIR})
endif()

# Optional headless smoke test (Linux/macOS)
if(NOT WIN32)
    add_executable(smoketest tools/smoketest.cpp)
    target_include_directories(smoketest PRIVATE clap/include)
    target_link_libraries(smoketest PRIVATE dl m)
endif()
