#!/usr/bin/env bash
# Run every scenario config in parallel. Each job gets its own -metadir AND its own
# java.io.tmpdir so neither TLC's time-stamped metadata dir nor its /tmp standard-
# module extraction collides across concurrent instances. Safety-only => -deadlock.
cd ~/Embarcadero-sessions/02-tla-spec
CFGS="core crash_pre_post_pbr retransmit_race dup_in_guard_window lease_false_positive stale_cv_ack_relay stale_cv_bug_demo client_crash_midstream seq_failover_open_gap module_loss_ack_window reader_agreement exactly_once_fence"
rm -rf states
for c in $CFGS; do
  mkdir -p "states/$c/tmp"
  java -Xmx32g -Djava.io.tmpdir="$PWD/states/$c/tmp" -XX:+UseParallelGC \
    -cp tla2tools.jar tlc2.TLC -deadlock -workers 20 \
    -metadir "states/$c" -config "spec/$c.cfg" spec/MCEmbarcadero.tla > "spec/results/$c.txt" 2>&1 &
done
wait
echo "ALL_DONE"
