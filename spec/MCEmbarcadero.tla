------------------------------ MODULE MCEmbarcadero ------------------------------
(***************************************************************************)
(* Thin model harness for TLC.  Every scenario .cfg runs this module,       *)
(* which EXTENDS the core spec and adds a symmetry set + a state constraint  *)
(* to keep the reachable state space tractable.  Constant values (Clients,  *)
(* Brokers, MaxSeq, GOICap, adversary flags) are supplied per-scenario in    *)
(* the .cfg files.                                                          *)
(***************************************************************************)
EXTENDS Embarcadero, TLC

\* Clients and Brokers are interchangeable model values -> symmetry reduction.
Symmetry == Permutations(Clients) \cup Permutations(Brokers)

\* Belt-and-suspenders bound (the state space is already finite): cap the log
\* and each PBR ring at the maximum number of distinct batches x broker copies.
StateConstraint ==
    /\ Len(goi) <= Cardinality(Clients) * MaxSeq
    /\ \A b \in Brokers : Len(pbr[b]) <= Cardinality(Clients) * MaxSeq

=============================================================================
