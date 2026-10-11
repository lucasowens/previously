# previously_render: offline WAV rendering through the engine (tier T3 golden tests, A/B listening).

# WAV reading and writing, shared by the render tool and its unit tests.
add_library(previously_wav STATIC
    wav.cpp
)

target_include_directories(previously_wav PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

previously_apply_strict_options(previously_wav)
previously_apply_embedded_subset(previously_wav)

add_executable(previously_render
    main.cpp
)

target_link_libraries(previously_render PRIVATE
    previously_wav
    previously::engine
    previously::host_support
)

previously_apply_strict_options(previously_render)
previously_apply_embedded_subset(previously_render)
