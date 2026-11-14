add_library(aether_animation STATIC
    one_euro.cpp
    kalman.cpp
    visibility_first.cpp
    ik_solvers.cpp
    spring_bone.cpp
    expression_mixer.cpp
    sim_engine.cpp
)

target_link_libraries(aether_animation
    PUBLIC
        aether_core
)

target_include_directories(aether_animation
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
)

if (MSVC)
    set_source_files_properties(spring_bone.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
endif()

aether_apply_cxx_flags(aether_animation)
