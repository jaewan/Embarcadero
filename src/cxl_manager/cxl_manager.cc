#include "cxl_manager.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <iostream>

namespace Embarcadero{

#define CXL_SIZE (1UL << 34)

CXLManager::CXLManager(){
	// Initialize CXL
	cxl_type_ = Emul;
	std::string cxl_path(getenv("HOME"));
	cxl_path += "/.CXL_EMUL/cxl";

	long cacheline_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

	switch(cxl_type_){
		case Emul:
			cxl_emul_fd_ = open(cxl_path.c_str(), O_RDWR, 0777);
			if (cxl_emul_fd_  < 0)
				perror("Opening Emulated CXL error");

			cxl_addr_= mmap(NULL, CXL_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, cxl_emul_fd_, 0);
			if (cxl_addr_ == MAP_FAILED)
				perror("Mapping Emulated CXL error");

			std::cout << "Successfully initialized CXL Emul" << std::endl;
			break;
		case Real:
			perror("Not implemented real cxl yet");
			break ;
	}
}

CXLManager::~CXLManager(){
	switch(cxl_type_){
		case Emul:
			if (munmap(cxl_addr_, CXL_SIZE) < 0)
				perror("Unmapping Emulated CXL error");
			close(cxl_emul_fd_);

			std::cout << "Successfully deinitialized CXL Emul" << std::endl;
			break;
		case Real:
			perror("Not implemented real cxl yet");
			break;
	}
}

} // End of namespace Embarcadero
