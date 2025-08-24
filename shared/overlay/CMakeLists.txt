cmake_minimum_required (VERSION 3.24)

project (hlae_overlay)

add_library(${PROJECT_NAME} STATIC)

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

if (MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${PROJECT_NAME} PUBLIC shell32.lib)
endif()
