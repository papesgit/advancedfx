cmake_minimum_required (VERSION 3.24)

project (hlae_overlay)

add_library(${PROJECT_NAME} STATIC)

# ---- Optional FreeType integration for Dear ImGui ----
option(HLAE_OVERLAY_WITH_FREETYPE "Use FreeType for ImGui font baking" ON)

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

target_sources(${PROJECT_NAME} PRIVATE
    IOverlayRenderer.h
    Overlay.h
    Overlay.cpp
    OverlayDx11.h
    OverlayDx11.cpp
    OverlayVk.h
    InputRouter.h
    InputRouter.cpp

    third_party/imgui/imgui.h
    ${HLAE_IMGUI_CORE_SOURCES}
    ${HLAE_IMGUI_FREETYPE_SOURCES}
    third_party/imgui/backends/imgui_impl_win32.h
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.h
    third_party/imgui/backends/imgui_impl_dx11.cpp
    
    # ImGui Neo Sequencer (third-party timeline widget)
    third_party/imgui_neo_sequencer/imgui_neo_sequencer.h
    third_party/imgui_neo_sequencer/imgui_neo_sequencer.cpp
    third_party/imgui_neo_sequencer/imgui_neo_internal.h
    third_party/imgui_neo_sequencer/imgui_neo_internal.cpp
    third_party/imguizmo/ImGuizmo.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui_neo_sequencer
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imguizmo
)

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
