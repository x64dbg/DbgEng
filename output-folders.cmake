# Keep the standalone build's existing output layout unless a parent project
# requests an engine-specific subdirectory.
if(DBGENG_OUTPUT_SUBDIR)
    function(dbgeng_set_nested_output_directory TARGET SUBDIR)
        if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
            set_target_properties(${TARGET} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${SUBDIR}"
                ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/${SUBDIR}"
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${SUBDIR}"
            )
        elseif(CMAKE_CONFIGURATION_TYPES)
            foreach(CONFIG ${CMAKE_CONFIGURATION_TYPES})
                string(TOUPPER ${CONFIG} CONFIG_UPPER)
                set_target_properties(${TARGET} PROPERTIES
                    RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER}
                        "${CMAKE_CURRENT_BINARY_DIR}/${CONFIG}/${SUBDIR}"
                    ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER}
                        "${CMAKE_CURRENT_BINARY_DIR}/${CONFIG}/${SUBDIR}"
                    LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER}
                        "${CMAKE_CURRENT_BINARY_DIR}/${CONFIG}/${SUBDIR}"
                )
            endforeach()
        else()
            set_target_properties(${TARGET} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${SUBDIR}"
                ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${SUBDIR}"
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${SUBDIR}"
            )
        endif()
    endfunction()

    dbgeng_set_nested_output_directory(DbgEngTitanEngine "${DBGENG_OUTPUT_SUBDIR}")
endif()
