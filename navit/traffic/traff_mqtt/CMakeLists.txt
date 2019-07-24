#Currently supported only on Linux
IF(traffic/traff_mqtt)


    include(ExternalProject)

    set(EXTERNAL_INSTALL_LOCATION ${CMAKE_BINARY_DIR}/pahoc)
    
    if(WIN32)
    ExternalProject_Add(pahoc
        GIT_REPOSITORY https://github.com/OLFDB/paho.mqtt.c.git
        CMAKE_ARGS -DPAHO_BUILD_STATIC=TRUE -DPAHO_WITH_SSL=TRUE -DPAHO_ENABLE_TESTING=FALSE -DCMAKE_INSTALL_PREFIX=${EXTERNAL_INSTALL_LOCATION}      
    )
    else(WIN32)
    ExternalProject_Add(pahoc
        GIT_REPOSITORY https://github.com/OLFDB/paho.mqtt.c.git
        CMAKE_ARGS  -DPAHO_WITH_SSL=TRUE -DPAHO_ENABLE_TESTING=FALSE -DCMAKE_INSTALL_PREFIX=${EXTERNAL_INSTALL_LOCATION}
    )
     endif(WIN32)
     
    include_directories(${EXTERNAL_INSTALL_LOCATION}/include)
    
    ExternalProject_Get_Property(pahoc install_dir)
    
    if(WIN32)
        add_library(mqtt STATIC IMPORTED)
        set_property(TARGET mqtt PROPERTY IMPORTED_LOCATION ${install_dir}/src/pahoc-build/src/libpaho-mqtt3c-static.a)
    else(WIN32)
        add_library(mqtt SHARED IMPORTED)
        set_property(TARGET mqtt PROPERTY IMPORTED_LOCATION ${install_dir}/src/pahoc-build/src/libpaho-mqtt3c.so)
    endif(WIN32)
    
    add_dependencies(mqtt pahoc)
    module_add_library(traffic_traff_mqtt traffic_traff_mqtt.c)
    target_link_libraries(traffic_traff_mqtt mqtt)
    
    if(WIN32)
        target_link_libraries(traffic_traff_mqtt Rpcrt4)
        target_link_libraries(traffic_traff_mqtt Crypt32)
        target_link_libraries(traffic_traff_mqtt ws2_32)
    else(WIN32)
        target_link_libraries(traffic_traff_mqtt uuid)
    endif(WIN32)
    
ENDIF (traffic/traff_mqtt)
