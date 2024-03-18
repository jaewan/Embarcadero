#ifndef INCLUDE_CXL_MANGER_H_
#define INCLUDE_CXL_MANGER_H_

#include "config.h"

namespace Embarcadero{

enum CXL_Type {Emul, Real};

class CXLManager{
	public:
		CXLManager();
		~CXLManager();

	private:
		CXL_Type cxl_type_;
		int cxl_emul_fd_;
		void* cxl_addr_;

};

} // End of namespace Embarcadero
#endif
