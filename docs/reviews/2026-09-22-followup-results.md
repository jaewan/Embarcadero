# Remaining refactoring work: follow-up

Status: implementation and qualification in progress. Prior source09 evidence is
retained separately and does not qualify the new extracted binaries.

Implemented: broker ordering/commit/scanning/recovery/export and client session/
retention/ACK compilation boundaries; owned experiment dispatch for11 historical
launchers; three additional live fault cases; linked actual Topic publication and
ChainReplicationManager disk-failure fixtures; per-process TSan workaround;
private GitHub vulnerability reporting enabled; paper implementation pointer
claim corrected to distinguish the common-address prototype from offset-only design.

Validation and final measurements will be recorded here after source freeze.
The [fixed measurement protocol](2026-09-22-followup-performance-protocol.md)
precedes the follow-up performance results.
