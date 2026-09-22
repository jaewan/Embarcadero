#include "common/support_contract.h"
#include <iostream>
#include <stdexcept>

using namespace Embarcadero;
static void Check(bool value, const char* name) {
    if (!value) throw std::runtime_error(name);
}

int main() {
    try {
        using Seq = SupportedSequencer;
        for (auto seq : {Seq::Embarcadero, Seq::Kafka, Seq::Scalog, Seq::Corfu, Seq::LazyLog}) {
            for (int order = -1; order <= 6; ++order) {
                const bool expected = seq == Seq::Scalog ? order == 1 :
                    (seq == Seq::Corfu || seq == Seq::LazyLog) ? order == 2 :
                    (order == 0 || order == 2 || order == 4 || order == 5);
                Check(ValidateSupportedMode({seq, order, 1, 0}, false).ok == expected,
                      "sequencer-specific order support");
            }
        }
        Check(!ValidateSupportedMode({static_cast<Seq>(99), 5, 1, 0}, false).ok, "unknown sequencer");
        for (int ack = -1; ack <= 3; ++ack) {
            for (int rf = -1; rf <= 3; ++rf) {
                const bool expected = ack >= 0 && ack <= 2 && rf >= 0 && (ack != 2 || rf >= 2);
                Check(ValidateSupportedMode({Seq::Embarcadero, 5, ack, rf}, false).ok == expected,
                      "ACK/RF policy");
            }
        }
        Check(!ValidateSupportedMode({Seq::Scalog, 1, 2, 2}, false).ok, "Scalog ACK2 requires CXL path");
        Check(ValidateSupportedMode({Seq::Scalog, 1, 2, 2}, true).ok, "Scalog valid research mode retained");
        Check(ValidateReplicationTopology(3, 3, {2, 0, 1}).ok, "unordered membership snapshot");
        Check(!ValidateReplicationTopology(2, 3, {0, 1}).ok, "partial membership cannot choose a different modulo topology");
        Check(ValidateReplicationTopology(2, 3, {0, 1, 2}).ok, "complete replicated topology");
        Check(!ValidateReplicationTopology(4, 8, {0, 1, 2}).ok, "insufficient distinct members");
        Check(!ValidateReplicationTopology(2, 1, {0, 1}).ok, "configured capacity");
        Check(!ValidateReplicationTopology(2, 8, {0, 1, 1}).ok, "duplicate identity");
        Check(!ValidateReplicationTopology(2, 8, {0, 2, 3}).ok, "hole in modulo topology");
        Check(!ValidateReplicationTopology(0, 8, {}).ok, "membership unavailable");
        Check(!ValidateReplicationTopology(0, 8, {-1, 0}).ok, "invalid identity");
        Check(ValidateReplicationTopology(0, 8, {0, 2, 3}).ok, "nonreplicated membership may have holes");
        Check(ValidateChainAdmission(5, 0, false, false, 0, 0, 1).ok, "RF0 needs no chain");
        Check(ValidateChainAdmission(5, 1, false, false, 0, 0, 1).ok, "RF1 primary only");
        Check(!ValidateChainAdmission(5, 2, false, false, 0, 0, 3).ok, "ACK-independent missing chain");
        Check(!ValidateChainAdmission(5, 2, false, true, 3, 3, 3).ok, "chain RF mismatch");
        Check(!ValidateChainAdmission(5, 0, false, true, 2, 3, 3).ok, "unrequested running chain");
        Check(!ValidateChainAdmission(5, 2, false, true, 2, 4, 3).ok, "chain membership mismatch");
        Check(ValidateChainAdmission(5, 2, false, true, 2, 3, 3).ok, "matched chain");
        Check(ValidateChainAdmission(5, 2, true, false, 0, 0, 3).ok, "explicit unified research path retained");
        Check(ValidateChainAdmission(0, 2, false, false, 0, 0, 3).ok, "legacy replication does not require GOI chain");
        std::cout << "support contract tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
