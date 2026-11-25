cmake_minimum_required(VERSION 3.22)
project(GVFG_CardTest LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Build GVFG_CardTest on Windows with MSVC.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)

if(CMAKE_GENERATOR MATCHES "Ninja" AND CMAKE_CL_SHOWINCLUDES_PREFIX MATCHES "^瘜冽")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含檔案: ")
endif()

set(sdk "${CMAKE_CURRENT_SOURCE_DIR}/../GVFG_Standalone/GVFG_SDK")
set(boot "${CMAKE_CURRENT_SOURCE_DIR}/../boot_cycle_test/source")
add_subdirectory("${sdk}" sdk EXCLUDE_FROM_ALL)

qt_add_executable(GVFG_CardTest WIN32
    main.cpp
    card_test_window.ui
    card_checks.cpp
    "${sdk}/sdk/gvfg/src/gigabyte/gigabyte_device.cpp"
    reports_dialog.cpp
    session.cpp
    card_startup_manager.cpp
    "${boot}/relay_agent.cpp"
    "${boot}/relay_controller.cpp"
)
set_target_properties(GVFG_CardTest PROPERTIES AUTOUIC ON)
set_property(SOURCE main.cpp APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/card_test_window.ui")
target_include_directories(GVFG_CardTest PRIVATE "${boot}" "${sdk}/sdk/gvfg/src"
    "${sdk}/third_party/GigabyteLib")
target_compile_options(GVFG_CardTest PRIVATE /W4 /utf-8)
target_compile_definitions(GVFG_CardTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(GVFG_CardTest PRIVATE Qt6::Widgets Qt6::Network gvfg setupapi advapi32 ole32 shell32 uuid)

install(TARGETS GVFG_CardTest gvfg RUNTIME DESTINATION bin)
install(FILES README.md DESTINATION .)
qt_generate_deploy_app_script(TARGET GVFG_CardTest OUTPUT_SCRIPT deploy_script NO_TRANSLATIONS)
install(SCRIPT "${deploy_script}")
