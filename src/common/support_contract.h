#pragma once

#include <algorithm>
#include <string>
#include <vector>
#include "common/ack_rf_policy.h"
#include "common/order_level.h"

namespace Embarcadero {

// Wire values from heartbeat.proto; production adapters static_assert these
// against the generated enum. This policy needs no protobuf or baseline code.
enum class SupportedSequencer { Embarcadero = 0, Kafka = 1, Scalog = 2, Corfu = 3, LazyLog = 4 };

struct SupportedMode {
    SupportedSequencer sequencer;
    int order;
    int ack;
    int replication_factor;
};

struct SupportValidation {
    bool ok;
    std::string error;
};

inline SupportValidation ValidateSupportedMode(const SupportedMode& mode, bool scalog_cxl_mode) {
    const auto ack = ValidateAckReplicationPolicy(mode.ack, mode.replication_factor);
    if (!ack.ok) return {false, ack.error};
    switch (mode.sequencer) {
        case SupportedSequencer::Embarcadero:
        case SupportedSequencer::Kafka: // Historical in-tree delegation ablation, not Apache Kafka.
            if (!IsCanonicalOrderLevel(mode.order))
                return {false, "Embarcadero ordering must be 0, 2, 4, or 5; ORDER1/3 are not implemented"};
            break;
        case SupportedSequencer::Scalog:
            if (mode.order != kOrderPerBroker)
                return {false, "Scalog requires ORDER1"};
            if (mode.ack == 2 && !scalog_cxl_mode)
                return {false, "Scalog ACK2 requires SCALOG_CXL_MODE=1"};
            break;
        case SupportedSequencer::Corfu:
        case SupportedSequencer::LazyLog:
            if (mode.order != kOrderTotal)
                return {false, "Corfu and LazyLog require ORDER2"};
            break;
        default: return {false, "unknown sequencer"};
    }
    return {true, {}};
}

inline SupportValidation ValidateReplicationTopology(int rf, int capacity, std::vector<int> live_ids) {
    if (rf < 0 || capacity <= 0 || rf > capacity)
        return {false, "replication factor exceeds configured broker capacity or is invalid"};
    if (live_ids.empty()) return {false, "live broker membership is not available"};
    std::sort(live_ids.begin(), live_ids.end());
    for (size_t i = 0; i < live_ids.size(); ++i) {
        if (live_ids[i] < 0 || live_ids[i] >= capacity || (i && live_ids[i] == live_ids[i - 1]))
            return {false, "live broker membership contains invalid or duplicate identities"};
        if (rf > 0 && live_ids[i] != static_cast<int>(i))
            return {false, "replicated topics require contiguous broker IDs starting at zero; restart a quiescent cluster after membership loss"};
    }
    if (rf > 0 && static_cast<int>(live_ids.size()) != capacity)
        return {false, "replicated topics require every configured broker to be live; partial or dynamic membership is unsupported"};
    if (rf > static_cast<int>(live_ids.size()))
        return {false, "replication factor exceeds distinct live broker membership"};
    return {true, {}};
}

inline SupportValidation ValidateChainAdmission(int order, int rf, bool unified_path,
        bool chain_running, int chain_rf, int chain_brokers, int live_brokers) {
    if (rf < 0) return {false, "negative replication factor"};
    if (chain_running && rf != chain_rf)
        return {false, "topic replication factor conflicts with the running chain; use the same EMBARCADERO_REPLICATION_FACTOR"};
    if (order == kOrderStrong && rf >= kMinReplicationFactorForAck2 && !unified_path) {
        if (!chain_running)
            return {false, "ORDER5 replication requires a running chain; set EMBARCADERO_REPLICATION_FACTOR before broker startup"};
        if (chain_brokers != live_brokers)
            return {false, "running chain broker count differs from live membership; start all configured chain members before topic creation"};
    }
    return {true, {}};
}

}  // namespace Embarcadero
