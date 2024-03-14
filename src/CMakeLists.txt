configure_file(config.h.in config.h)

add_executable(embarlet embarlet/embarlet.cc disk_manager/disk_manager.cc config.h)  

target_include_directories(embarlet PUBLIC
							"${CMAKE_CURRENT_BINARY_DIR}"
						   )
