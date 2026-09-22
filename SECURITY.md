# Security and deployment scope

Embarcadero is a research prototype for a trusted, isolated Linux test network.
Its current control and data protocols do not provide authentication, encryption,
or authorization. Do not expose broker ports to untrusted networks or process
untrusted shared-memory files. Resource limits and parser checks are containment,
not a security boundary. Fault-injection builds are test-only and must never be
deployed as ordinary brokers.

Report reproducible correctness or security defects with the affected revision,
configuration, minimal reproducer, and sanitized logs. Do not put credentials,
private data, or a working sensitive exploit into a public issue. For sensitive
reports, use [GitHub private vulnerability reporting](https://github.com/jaewan/Embarcadero/security/advisories/new).
Private reporting is enabled for this repository. No response-time or supported-version commitment has yet been made.

Run development tooling as an unprivileged user. The isolated runner owns only
its own processes and region. Historical experiment scripts have different
cleanup contracts and are not the default supported workflow. See
[development instructions](docs/development-dram.md) for bounded DRAM use.
