#include "publisher.h"

Publisher::Publisher(PublisherConfig *config) 
{
    this->config = config;
}

PublisherError Publisher::publish(Topic *topic, int msgflags, void *payload, size_t len) {
    return ERR_NOT_IMPLEMENTED;
}

