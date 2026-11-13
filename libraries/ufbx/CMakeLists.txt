# ufbx (https://github.com/ufbx/ufbx, commit 5955c5c0b042ac2dc6f32955ab206aaec0620cfc), vendored as is:
# ufbx.c, ufbx.h, LICENSE (MIT or public domain). Read by src/common/models/fbx_skinned.cpp, the FBX
# model loader (model_fbx.h).
cmake_minimum_required(VERSION 3.11)

make_release_only()

add_library( ufbx STATIC ufbx.c )
target_include_directories( ufbx INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>" )
if (MSVC)
	target_compile_options( ufbx PRIVATE /wd4244 /wd4267 )
endif()
