# cgltf v1.15 (https://github.com/jkuhlmann/cgltf, tag v1.15), MIT: see LICENSE. A single-header glTF 2.0 parser; the
# implementation is compiled once, in pcsx2/VR/HandsGltf.cpp (CGLTF_IMPLEMENTATION). Unmodified; README.pcsx2 has
# the file's sha256.
add_library(cgltf INTERFACE)
target_include_directories(cgltf INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include/")
