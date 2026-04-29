add_executable(mars
    main.cpp
)

target_link_libraries(mars
    PRIVATE
        mars::app
        mars::warnings
)

find_program(SETCAP_PATH setcap)
if(MARS_SETCAP_AFTER_BUILD AND SETCAP_PATH)
    add_custom_target(setcap_mars ALL
        COMMAND sudo ${SETCAP_PATH} "cap_sys_nice=eip cap_perfmon=eip" $<TARGET_FILE:mars>
        COMMENT "Enter password to set capabilities for realtime priority and PMC access"
        DEPENDS mars
    )
endif()
