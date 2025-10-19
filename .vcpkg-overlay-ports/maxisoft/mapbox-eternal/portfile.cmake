vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO mapbox/eternal
        REF "v${VERSION}"
        SHA512 bb3dbed2ceee3a6439efe1967af1e25b4cbbc54d8e28f9c8229e65f46310dd5a5a32cc777ae24f383a3a0b8b3991feb0bd3dc22faa0208a0fce23c6b67d21986
        HEAD_REF "v${VERSION}"
)

# Copy header files
file(COPY "${SOURCE_PATH}/include/" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

# Copy license file
file(INSTALL "${SOURCE_PATH}/LICENSE.md" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)