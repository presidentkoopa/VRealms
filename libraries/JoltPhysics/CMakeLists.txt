# Jolt Physics (https://github.com/jrouwe/JoltPhysics, tag v5.3.0), vendored as is: Jolt/ and LICENSE (MIT).
# Read by src/roth/roth_physics_core.cpp, VRealms' physical hands and objects (docs/VR_PHYSICS.md).
# Built with this project's own flags (no fast math, no /arch beyond the default), single precision, no debug
# renderer, profiler or object stream. The defines below are PUBLIC: Jolt checks at run time that the library and
# its users were built with the same configuration.
cmake_minimum_required(VERSION 3.11)

make_release_only()

file( GLOB_RECURSE JOLT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Jolt/*.cpp" )
add_library( Jolt STATIC ${JOLT_SOURCES} )
target_include_directories( Jolt PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>" )
set_target_properties( Jolt PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON )
target_compile_definitions( Jolt PUBLIC JPH_NO_FORCE_INLINE )
if( MSVC )
	target_compile_options( Jolt PRIVATE /fp:precise /bigobj /wd4100 /wd4127 /wd4244 /wd4267 )
else()
	target_compile_options( Jolt PRIVATE -fno-fast-math -ffp-contract=off -w )
endif()
