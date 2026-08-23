set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Force vcpkg onto the VS 2022 install (MSVC v143 / 14.44). vcpkg does its OWN Visual
# Studio detection and otherwise picks the NEWEST install (VS 2026 / 14.51), which is
# too new for pinned deps (e.g. fmt 10 uses the removed stdext::checked_array_iterator).
# MUST use backslashes: vcpkg does a literal path-equality match and forward slashes fail.
# Do NOT also set VCPKG_PLATFORM_TOOLSET (the path + toolset-name combo fails to match).
set(VCPKG_VISUAL_STUDIO_PATH "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools")
