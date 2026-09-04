cmake_minimum_required(VERSION 3.16)
project(ForAndSim CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FORANDSIM_BUILD_GUI "Build the Dear ImGui GUI front-end (requires SDL2)" ON)

if(WIN32)
  # windows.h defines min/max macros unless told not to, which breaks any
  # std::min/std::max call in a translation unit that includes it (MSVC error
  # C2589 "illegal token on right side of '::'"). Apply project-wide instead
  # of chasing every #include <windows.h>.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(MSVC)
  # Static CRT so the shipped .exe needs no VC++ redistributable install.
  foreach(flag_var
      CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
      CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO
      CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE)
    if(${flag_var} MATCHES "/MD")
      string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
    endif()
  endforeach()
endif()

# ---- core acquisition library (PC/SC transport, APDU, engine, output) ----

add_library(forandsim_core STATIC
  src/core/pcsc_transport.cpp
  src/core/apdu.cpp
  src/core/card_session.cpp
  src/core/ef_catalog.cpp
  src/core/file_walker.cpp
  src/core/acquisition_engine.cpp
  src/core/hashing.cpp
  src/core/time_utils.cpp
  src/core/platform_info.cpp
  src/core/reader_discovery.cpp
  src/core/output/report_common.cpp
  src/core/output/output_paths.cpp
  src/core/output/zip_writer.cpp
  src/core/output/html_report.cpp
  third_party/miniz.c
  third_party/miniz_tdef.c
  third_party/miniz_tinfl.c
  third_party/miniz_zip.c
)

target_include_directories(forandsim_core PUBLIC
  src/core
  src/core/output
  third_party
)

if(WIN32)
  target_link_libraries(forandsim_core PUBLIC winscard)
elseif(APPLE)
  target_link_libraries(forandsim_core PUBLIC "-framework PCSC")
else()
  find_library(PCSC_LIB pcsclite)
  if(PCSC_LIB)
    target_link_libraries(forandsim_core PUBLIC ${PCSC_LIB})
    target_include_directories(forandsim_core PUBLIC /usr/include/PCSC)
  else()
    target_link_libraries(forandsim_core PUBLIC pcsclite)
  endif()
endif()

# ---- CLI front-end (always built) ----

add_library(forandsim_cli STATIC src/cli/cli_app.cpp)
target_include_directories(forandsim_cli PUBLIC src/cli)
target_link_libraries(forandsim_cli PUBLIC forandsim_core)

# ---- GUI front-end (optional, needs SDL2) ----

if(FORANDSIM_BUILD_GUI)
  find_package(SDL2 QUIET)
  if(NOT SDL2_FOUND)
    message(WARNING "SDL2 not found; disabling GUI (CLI-only build). Install SDL2 dev package or vcpkg's sdl2 port to enable it.")
    set(FORANDSIM_BUILD_GUI OFF)
  endif()
endif()

set(FORANDSIM_TARGET_SOURCES src/main.cpp)

if(FORANDSIM_BUILD_GUI)
  add_library(forandsim_gui STATIC
    src/gui/main_gui.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_sdl2.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp
  )
  target_include_directories(forandsim_gui PUBLIC
    src/gui
    third_party/imgui
    third_party/imgui/backends
  )
  if(TARGET SDL2::SDL2)
    target_link_libraries(forandsim_gui PUBLIC forandsim_core SDL2::SDL2 SDL2::SDL2main)
  else()
    target_include_directories(forandsim_gui PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(forandsim_gui PUBLIC forandsim_core ${SDL2_LIBRARIES})
  endif()
  if(APPLE)
    target_link_libraries(forandsim_gui PUBLIC "-framework OpenGL")
  elseif(UNIX)
    target_link_libraries(forandsim_gui PUBLIC GL)
  else()
    target_link_libraries(forandsim_gui PUBLIC opengl32 ole32 shell32 uuid)
  endif()
endif()

# On Windows, a GUI-capable build links as a WINDOWS-subsystem exe so
# double-clicking it doesn't spawn a console window that can't be closed
# without killing the app. main.cpp re-attaches to the caller's console when
# invoked with CLI arguments (see AttachConsole there). MSVC's WINDOWS
# subsystem otherwise expects a WinMain, so force the normal CRT entry point.
if(WIN32 AND FORANDSIM_BUILD_GUI)
  add_executable(forandsim WIN32 ${FORANDSIM_TARGET_SOURCES})
  if(MSVC)
    # /MT above means we intend the static CRT (LIBCMT) throughout, but a
    # vcpkg-provided static lib can still declare MSVCRT (the dynamic CRT) as
    # its preferred default lib, producing an LNK4098 conflict warning.
    # Explicitly drop MSVCRT from the defaults so LIBCMT wins outright,
    # instead of leaving the ambiguity for the linker to warn about.
    set_target_properties(forandsim PROPERTIES
      LINK_FLAGS "/ENTRY:mainCRTStartup /NODEFAULTLIB:MSVCRT")
  endif()
else()
  add_executable(forandsim ${FORANDSIM_TARGET_SOURCES})
  if(MSVC)
    set_target_properties(forandsim PROPERTIES LINK_FLAGS "/NODEFAULTLIB:MSVCRT")
  endif()
endif()
target_link_libraries(forandsim PRIVATE forandsim_cli)
if(FORANDSIM_BUILD_GUI)
  target_compile_definitions(forandsim PRIVATE FORANDSIM_BUILD_GUI=1)
  target_link_libraries(forandsim PRIVATE forandsim_gui)
endif()

# ---- tests ----

enable_testing()
add_executable(forandsim_output_smoke_test tests/output_smoke_test.cpp)
target_link_libraries(forandsim_output_smoke_test PRIVATE forandsim_core)
add_test(NAME output_smoke_test COMMAND forandsim_output_smoke_test)
