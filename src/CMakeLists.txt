configure_file(config.h.in config.h)

add_executable(embarlet embarlet/embarlet.cc disk_manager/disk_manager.cc network_manager/network_manager.cc network_manager/network_manager.h config.h)

target_include_directories(embarlet PUBLIC
							"${CMAKE_CURRENT_BINARY_DIR}"
						   )

target_link_libraries(embarlet absl::strings)