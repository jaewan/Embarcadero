#pragma once

#include "subscriber.h"

namespace Embarcadero::client {

// Completion policy shared by the latency drain and its ACK-primary reporting.
// A missing population may be a timeout; a terminal consumer failure is never
// evidence of successful delivery, even if the publisher received every ACK.
struct DeliveryCompletion {
    bool timed_out = false;
    bool terminal_delivery_error = false;

    bool ObserveOrderedStatus(const Subscriber::OrderedDeliveryStatus& status) {
        terminal_delivery_error = terminal_delivery_error ||
            status.retention_exhausted || status.stopped ||
            status.parse_errors != 0 || status.export_gaps != 0 ||
            status.duplicates != 0;
        return terminal_delivery_error;
    }

    bool MayUseAckPrimary(bool ack_primary, bool hard_ordering_fault,
                          bool duplicate_uid) const {
        return ack_primary && !terminal_delivery_error && !hard_ordering_fault &&
            (timed_out || duplicate_uid);
    }
};

}  // namespace Embarcadero::client
