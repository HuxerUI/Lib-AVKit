set(HUXERUI_MACOS_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in")
set(HUXERUI_MACOS_APP_ICON "${CMAKE_CURRENT_LIST_DIR}/AppIcon.icns")

function(huxerui_configure_macos_project_package target_name install_component)
    target_sources(${target_name} PRIVATE "${HUXERUI_MACOS_APP_ICON}")
    set_property(TARGET ${target_name} PROPERTY MACOSX_BUNDLE_ICON_FILE "AppIcon")
    set_source_files_properties("${HUXERUI_MACOS_APP_ICON}" PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources
    )
    if (HUXERUI_PACKAGE)
        install(TARGETS ${target_name}
                BUNDLE DESTINATION .
                COMPONENT "${install_component}"
        )
        _huxerui_install_runtime_dependencies(${target_name} "${install_component}"
                "$<TARGET_FILE_NAME:${target_name}>.app/Contents" "MacOS/$<TARGET_FILE_NAME:${target_name}>"
        )
    endif ()
endfunction()

function(avkit_embed_macos_pag target_name)
    if (AVKIT_PAG_LIBRARY MATCHES "^(.*\\.framework)/")
        set(avkit_pag_framework "${CMAKE_MATCH_1}")
        get_filename_component(avkit_pag_framework_name "${avkit_pag_framework}" NAME)
        set_property(TARGET ${target_name} APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
        add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND /usr/bin/ditto "${avkit_pag_framework}"
                    "$<TARGET_BUNDLE_DIR:${target_name}>/Contents/Frameworks/${avkit_pag_framework_name}"
                VERBATIM)
    endif ()
endfunction()
