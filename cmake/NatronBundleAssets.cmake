option(NATRON_BUNDLE_ASSETS "Install prebuilt OFX plugin bundles and OCIO configs from CMAKE_BINARY_DIR/assets into the Natron bundle layout" OFF)

if(NATRON_BUNDLE_ASSETS)
    set(NATRON_ASSETS_DIR "${CMAKE_BINARY_DIR}/assets")

    if(NOT EXISTS "${NATRON_ASSETS_DIR}/Plugins")
        message(FATAL_ERROR "NATRON_BUNDLE_ASSETS is ON but ${NATRON_ASSETS_DIR}/Plugins not found. Run tools/ci/local/fetch-assets.sh first.")
    endif()

    if(NOT EXISTS "${NATRON_ASSETS_DIR}/OpenColorIO-Configs")
        message(FATAL_ERROR "NATRON_BUNDLE_ASSETS is ON but ${NATRON_ASSETS_DIR}/OpenColorIO-Configs not found. Run tools/ci/local/fetch-assets.sh first.")
    endif()

    file(GLOB NATRON_OFX_BUNDLES "${NATRON_ASSETS_DIR}/Plugins/*.ofx.bundle")
    foreach(NATRON_OFX_BUNDLE ${NATRON_OFX_BUNDLES})
        install(DIRECTORY "${NATRON_OFX_BUNDLE}"
            DESTINATION "Plugins/OFX/Natron"
            USE_SOURCE_PERMISSIONS
        )
    endforeach()

    install(DIRECTORY "${NATRON_ASSETS_DIR}/OpenColorIO-Configs"
        DESTINATION "Resources"
    )

    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Gui/Resources/etc/fonts/fonts.conf.in" fonts_conf_content)
    string(REPLACE "$$FC_DEFAULT_FONTS" "<dir>/usr/share/fonts</dir>" fonts_conf_content "${fonts_conf_content}")
    string(REPLACE "$$FC_CACHEDIR" "<cachedir>/var/cache/fontconfig</cachedir>" fonts_conf_content "${fonts_conf_content}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fonts.conf" "${fonts_conf_content}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/fonts.conf"
        DESTINATION "Resources/etc/fonts"
    )
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Gui/Resources/etc/fonts/conf.d"
        DESTINATION "Resources/etc/fonts"
    )

    # AppManager.cpp:1708's absolute /usr/share/Natron/Plugins is deliberately unsatisfied by a relocatable prefix
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Gui/Resources/PyPlugs/"
        DESTINATION "Plugins"
    )

endif()
