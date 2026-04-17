include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

function(deflate_install target)
    install(TARGETS ${target}
        EXPORT deflateTargets
        LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/deflate
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    )

    install(EXPORT deflateTargets
        FILE deflateTargets.cmake
        NAMESPACE deflate::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/deflate
    )

    configure_package_config_file(
        ${CMAKE_SOURCE_DIR}/cmake/deflateConfig.cmake.in
        ${CMAKE_BINARY_DIR}/deflateConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/deflate
    )

    write_basic_package_version_file(
        ${CMAKE_BINARY_DIR}/deflateConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_BINARY_DIR}/deflateConfig.cmake
        ${CMAKE_BINARY_DIR}/deflateConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/deflate
    )

    # pkg-config
    configure_file(
        ${CMAKE_SOURCE_DIR}/cmake/deflate.pc.in
        ${CMAKE_BINARY_DIR}/deflate.pc
        @ONLY
    )
    install(FILES ${CMAKE_BINARY_DIR}/deflate.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )
endfunction()
