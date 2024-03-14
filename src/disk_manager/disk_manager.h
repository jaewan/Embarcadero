#ifndef INCLUDE_DISK_MANGER_H_
#define INCLUDE_DISK_MANGER_H_

#include <cmath>
#include <iostream>
#include <string>
#include "config.h"

namespace Embarcadero{

enum CXL_Type {Emul, Real};

class DiskManager{
	public:
		DiskManager();
		~DiskManager();

	private:
		CXL_Type cxl_type_;
		int cxl_emul_fd_;
		void* cxl_addr_;

};

} // End of namespace Embarcadero
#endif
