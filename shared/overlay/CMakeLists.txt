cmake_minimum_required (VERSION 3.24)

project (hlae_overlay)

add_library(${PROJECT_NAME} STATIC)

# ---- Optional FreeType integration for Dear ImGui ----
option(HLAE_OVERLAY_WITH_FREETYPE "Use FreeType for ImGui font baking" OFF)

# Try to use official Dear ImGui core sources if present, else fall back to stub.
set(HLAE_IMGUI_CORE_SOURCES)
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp)
    list(APPEND HLAE_IMGUI_CORE_SOURCES
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/imgui_tables.cpp
    )
else()
    message(WARNING "Overlay: Using stub Dear ImGui; drop official imgui_* sources into shared/overlay/third_party/imgui to enable rendering.")
    list(APPEND HLAE_IMGUI_CORE_SOURCES
        third_party/imgui/imgui.cpp
    )
endif()

# Optional: Add the FreeType font builder backend for ImGui.
set(HLAE_IMGUI_FREETYPE_SOURCES)
if (HLAE_OVERLAY_WITH_FREETYPE)
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/misc/freetype/imgui_freetype.cpp)
        list(APPEND HLAE_IMGUI_FREETYPE_SOURCES
            third_party/imgui/misc/freetype/imgui_freetype.cpp
        )
    else()
        message(WARNING "Overlay: HLAE_OVERLAY_WITH_FREETYPE=ON but imgui_freetype.cpp not found. Disabling FreeType backend for this target.")
        set(HLAE_OVERLAY_WITH_FREETYPE OFF)
    endif()
endif()

set(HLAE_OVERLAY_SOURCES
    IOverlayRenderer.h
    Overlay.h
    Overlay.cpp
    AttachCameraState.h
    AttachCameraState.cpp
    OverlayVk.h
    InputRouter.h
    InputRouter.cpp
    Hud.h
    Hud.cpp
    GsiHttpServer.h
    GsiHttpServer.cpp

    radar/Radar.h
    radar/Radar.cpp

    third_party/nanosvg/nanosvg.h
    third_party/nanosvg/nanosvgrast.h

    third_party/imgui/imgui.h
    ${HLAE_IMGUI_CORE_SOURCES}
    ${HLAE_IMGUI_FREETYPE_SOURCES}
    third_party/imgui/backends/imgui_impl_win32.h
    third_party/imgui/backends/imgui_impl_win32.cpp

    # ImGui Neo Sequencer (third-party timeline widget)
    third_party/imgui_neo_sequencer/imgui_neo_sequencer.h
    third_party/imgui_neo_sequencer/imgui_neo_sequencer.cpp
    third_party/imgui_neo_sequencer/imgui_neo_internal.h
    third_party/imgui_neo_sequencer/imgui_neo_internal.cpp
    third_party/imguizmo/ImGuizmo.cpp
)

# Add DX9 backend only on 32-bit (Source 1)
if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    list(APPEND HLAE_OVERLAY_SOURCES
        OverlayDx9.h
        OverlayDx9.cpp
        third_party/imgui/backends/imgui_impl_dx9.h
        third_party/imgui/backends/imgui_impl_dx9.cpp)
endif()

# Add DX11 backend only on 64-bit (Source 2)
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND HLAE_OVERLAY_SOURCES
        OverlayDx11.h
        OverlayDx11.cpp
        third_party/imgui/backends/imgui_impl_dx11.h
        third_party/imgui/backends/imgui_impl_dx11.cpp)
endif()

target_sources(${PROJECT_NAME} PRIVATE ${HLAE_OVERLAY_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui_neo_sequencer
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imguizmo
)

# For 32-bit (Source 1), allow DX9 overlay to include AfxHookSource headers and deps.
if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/AfxHookSource
        ${CMAKE_SOURCE_DIR}/deps/release/prop
        ${CMAKE_SOURCE_DIR}/deps/release/prop/AfxHookSource
    )
endif()

target_compile_definitions(${PROJECT_NAME} PUBLIC HUD_USE_NANOSVG)

# If FreeType is enabled, add include, define, find/link the library.
if (HLAE_OVERLAY_WITH_FREETYPE)
    # ImGui's FreeType backend header lives here; including it is harmless even if not directly used.
    target_include_directories(${PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/misc/freetype
    )

    # Define the macro so ImGui uses the FreeType font builder
    target_compile_definitions(${PROJECT_NAME} PUBLIC IMGUI_ENABLE_FREETYPE)

    # Prefer an installed package first
    find_package(Freetype QUIET)

    if (Freetype_FOUND)
        set(FREETYPE_TARGET Freetype::Freetype)
    else()
        include(FetchContent)

        # Optional: avoid noisy dependency lookups on Windows; FreeType builds fine without them for our use.
        set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON CACHE BOOL "" FORCE)
        set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB     ON CACHE BOOL "" FORCE)

        FetchContent_Declare(
            freetype
            GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
            GIT_TAG VER-2-13-2
        )
        # Build static and keep it lean
        set(FT_DISABLE_PNG         ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_BZIP2       ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_HARFBUZZ    ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_BROTLI      ON CACHE BOOL "" FORCE)
        set(FREETYPE_BUILD_TOOLS   OFF CACHE BOOL "" FORCE)
        set(FREETYPE_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)

        FetchContent_MakeAvailable(freetype)

        if (TARGET freetype)
            set(FREETYPE_TARGET freetype)
        elseif (TARGET Freetype::Freetype)
            set(FREETYPE_TARGET Freetype::Freetype)
        else()
            message(FATAL_ERROR "FreeType fetched but no CMake target found (expected 'freetype' or 'Freetype::Freetype').")
        endif()
    endif()

    target_link_libraries(${PROJECT_NAME} PRIVATE ${FREETYPE_TARGET})
endif()

if (MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${PROJECT_NAME} PUBLIC shell32.lib)
endif()

# Use safer default toggle keys on 32-bit (Source 1): F8 only; disable alt toggle
if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_definitions(${PROJECT_NAME} PUBLIC OVERLAY_TOGGLE_VK=0x77 OVERLAY_TOGGLE_ALT_VK=0)
endif()

# Install built-in radar assets alongside other resources so runtime can pick them automatically.
install(FILES assets/radar/radars.json DESTINATION "bin/resources/overlay/radar")
install(FILES assets/gamestate_integration_drweissbrot_hud.cfg DESTINATION "bin/resources")
install(DIRECTORY assets/radar/ingame DESTINATION "bin/resources/overlay/radar")
install(DIRECTORY assets/icons DESTINATION "bin/resources/overlay")
install(DIRECTORY assets/weapons DESTINATION "bin/resources/overlay")
