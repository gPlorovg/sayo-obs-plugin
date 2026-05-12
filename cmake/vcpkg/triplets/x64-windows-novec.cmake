set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# Workaround for unresolved __std_* vectorized STL symbols in grpc/abseil
# with some MSVC v143 toolset/runtime combinations.
set(VCPKG_C_FLAGS "/D_USE_STD_VECTOR_ALGORITHMS=0")
set(VCPKG_CXX_FLAGS "/D_USE_STD_VECTOR_ALGORITHMS=0")
