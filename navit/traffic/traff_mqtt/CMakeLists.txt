#Currently supported only on Linux
IF(traffic/traff_mqtt)
    include(ExternalProject)

    set(EXTERNAL_INSTALL_LOCATION ${CMAKE_BINARY_DIR}/pahoc)

    ExternalProject_Add(pahoc
        GIT_REPOSITORY https://github.com/eclipse/paho.mqtt.c.git
        CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${EXTERNAL_INSTALL_LOCATION}
    )

    include_directories(${EXTERNAL_INSTALL_LOCATION}/include)
    link_directories(${EXTERNAL_INSTALL_LOCATION}/lib/)

    module_add_library(traffic_traff_mqtt traffic_traff_mqtt.c)

    add_dependencies(traffic_traff_mqtt pahoc)
    target_link_libraries(traffic_traff_mqtt libpaho-mqtt3c.so)
ENDIF (traffic/traff_mqtt)
