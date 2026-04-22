add_executable(mars
    main.cpp
)

target_link_libraries(mars
    PRIVATE
        mars::app
        mars::warnings
)

find_program(SETCAP_PATH setcap)
if(SETCAP_PATH)
    add_custom_command(TARGET mars POST_BUILD
        COMMAND sudo ${SETCAP_PATH} cap_perfmon=eip $<TARGET_FILE:mars>
        COMMENT "Setting cap_perfmon capability for PMC access"
    )
endif()
