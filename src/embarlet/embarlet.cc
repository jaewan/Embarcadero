#include "config.h"
#include "pub_queue.h"
#include "pub_task.h"
#include "../disk_manager/disk_manager.h"
#include "../network_manager/network_manager.h"
//#include "../cxl_manager/cxl_manager.h"

int main(int argc, char* argv[]){
	folly::MPMCQueue<PubTask> pq = create_pub_queue();
	return 0;
}
