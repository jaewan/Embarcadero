#include "disk_manager.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <iostream>

namespace Embarcadero{

#define CXL_SIZE (1UL << 34)

DiskManager::DiskManager(){
}

DiskManager::~DiskManager(){
}

} // End of namespace Embarcadero
