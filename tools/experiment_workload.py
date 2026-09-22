#!/usr/bin/env python3
"""Bounded local DRAM workload variations sharing dev_cluster's owned lifecycle.

These are finite development experiments, not a matched-load latency study or
qualification of the historical multi-host scripts. No production hooks needed.
"""
import argparse
import csv
import math
import os
from pathlib import Path
import re
import sys
import time

import dev_cluster as dev


def one_row(path):
    with path.open() as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1:
        raise dev.RunError("expected exactly one result row: " + str(path))
    return rows[0]


def finite(row, names):
    for name in names:
        number = float(row[name])
        if not math.isfinite(number) or number <= 0:
            raise dev.RunError("missing or nonpositive measurement: " + name)


def ack_and_routing(log, count, allowed):
    ack = re.findall(r"\[ACK_VERIFY\] normalized_received=(\d+) raw_received=(\d+) target=(\d+) 100%", log)
    if len(ack) != 1 or int(ack[0][0]) != count or int(ack[0][2]) != count:
        raise dev.RunError("authoritative ACK frontier did not exactly complete the client workload")
    # raw_received can legally lag the authoritative ORDER5 HWM.
    routing = re.findall(r"\[ORDER5_ROUTING\] ([^\n]+)", log)
    if not routing:
        raise dev.RunError("missing final ORDER5 routing evidence")
    row = dict(re.findall(r"(\w+)=([^\s]+)", routing[-1]))
    sent = {int(k[6:-5]): int(v) for k, v in row.items() if re.fullmatch(r"broker\d+_msgs", k)}
    if (int(row['retransmit_attempts']) != 0 or int(row['session_fenced_observed']) != 0 or
            int(row['session_rto_min_ms']) != 60000 or sum(sent.values()) != count or
            not set(sent).issubset(set(allowed))):
        raise dev.RunError("routing escaped its allowed brokers, retried, fenced, or lost messages")
    if re.search(r"ACK Timeout|status=failed|Subscriber::Poll timeout|\[SESSION_FENCED_OBSERVED\]", log):
        raise dev.RunError("client reported timeout, fence, or a failed audit")
    return {"client_id": int(row['client_id']), "messages": count,
            "normalized_ack": int(ack[0][0]), "raw_ack_diagnostic": int(ack[0][1]), "sent_by_broker": sent}


class Workload:
    def __init__(self, args):
        self.args = args
        self.plans = []
        self.go_ns = None

    def prepare(self, config, env, selected, manifest, hardware, common, run_dir):
        args = self.args
        total = sum(args.payloads) * dev.MIB
        reserve = total // dev.MESSAGE_BYTES * (dev.MESSAGE_BYTES + 128) + 2 * dev.MIB * common.brokers * args.threads * args.clients
        if reserve >= dev.SEGMENT_BYTES - 4096:
            raise dev.RunError("bounded workload can exceed one broker's initial segment")
        required = dev.REGION_BYTES + (2 * common.brokers + 3 * args.clients) * dev.GIB
        if hardware['nodes']['0']['free_bytes'] < 3 * args.clients * dev.GIB:
            raise dev.RunError("workload requires 3 GiB node-0 memory headroom per client")
        if any(limit['remaining_bytes'] < required for limit in manifest['memory']['cgroup_limits']):
            raise dev.RunError("cgroup lacks the bounded workload's client memory headroom")
        manifest['memory']['additional_memory_budget_bytes'] = required
        config['embarcadero']['network']['io_threads'] = max(6, args.clients * args.threads + 8)
        # Keep the smoke's 2MiB batch/pool geometry and RTO floor. Latency mode's
        # independent runtime defaults are not silently substituted.
        selected.update(EMBARCADERO_LATENCY_ACK_PRIMARY='0', EMBAR_VALIDATE_ORDER='1' if args.kind == 'gap' else '0')
        env.update(selected)
        manifest.update(profile='dev-dram-workload-' + args.kind,
            application_payload_bytes=total, message_bytes=dev.MESSAGE_BYTES,
            initial_payload_reservation_upper_bound_bytes=reserve,
            client_deadline_seconds=60, workload={
                'kind': args.kind, 'independent_client_processes': args.clients,
                'payload_mib_per_client': args.payloads, 'target_mibps_per_client': args.rates,
                'broker_allowlists': args.allowlists, 'threads_per_broker': args.threads,
                'steady_rate': args.steady_rate, 'gap_delay_ms': args.gap_ms if args.kind == 'gap' else None,
                'timing_scope': 'finite client transfer; setup and owned cleanup excluded from client-reported rates',
                'pacing_bytes': 'payload rounded to 64 bytes plus native MessageHeader, as implemented by the client',
                'latency_scope': 'payload timestamp is written before pacing; includes that scheduling wait',
                'publisher_ack_latency_instrumentation': False,
                'validation_scope': ('exact indexed single-session delivery after a logged sender delay' if args.kind == 'gap' else
                    'exact global-position and UID-population counters plus finite delivery telemetry; no payload comparison or per-session FIFO proof' if args.kind == 'latency' else
                    'independent session identities, exact per-client ACK completion and routing; no combined subscriber payload audit')})
        manifest['limitations'] = [value for value in manifest['limitations'] if not value.startswith('RTO floor exceeds')]
        manifest['limitations'].extend([
            'RTO floor equals the 60-second whole-client deadline; qualification explicitly requires zero retransmissions.',
            'No fixed-offered-load latency equivalence, sustained capacity, or multi-host claim.',
            'Broker allowlists select destinations; they do not implement a statistical key-skew distribution.',
            'A sender-gap marker proves delay injection, not that a later batch overtook it.',
            'Finite percentile telemetry is not a performance comparison; publisher ACK latency instrumentation is not enabled.'])

    def commands(self, binding, binary, config, run_dir):
        args = self.args
        for index in range(args.clients):
            directory = run_dir / f'client-{index}'
            directory.mkdir()
            kind = {'gap': 1, 'latency': 2, 'publishers': 5}[args.kind]
            command = binding + [str(binary), '--config', str(config), '--head_addr', '127.0.0.1',
                '-t', str(kind), '-o', '5', '-a', '1', '-r', '0', '-n', str(args.threads),
                '-m', str(dev.MESSAGE_BYTES), '-s', str(args.payloads[index] * dev.MIB), '--sequencer', 'EMBARCADERO']
            environment = {'EMBARCADERO_DATA_DIR': str(directory / 'results') + '/',
                'EMBARCADERO_PUBLISH_BROKER_ALLOWLIST': ','.join(map(str, args.allowlists[index]))}
            if args.kind != 'gap':
                command += ['--target_mbps', str(args.rates[index])]
                if args.steady_rate:
                    command += ['--steady_rate']
            if args.kind == 'gap':
                environment.update(EMBARCADERO_ORDER5_GAP_BATCH_SEQ='0',
                    EMBARCADERO_ORDER5_GAP_DELAY_MS=str(args.gap_ms), EMBARCADERO_ORDER5_GAP_PERIOD_BATCHES='0')
            if args.kind == 'publishers':
                environment.update(EMBARCADERO_PUSH_READY_FILE=str(directory / 'ready'),
                    EMBARCADERO_PUSH_GO_FILE=str(run_dir / 'publish-go-ns'))
            self.plans.append({'name': f'client-{index}', 'command': command,
                               'cwd': str(directory), 'environment': environment})
        return self.plans

    def execute(self, owned, manifest, hardware, run_dir):
        started = time.monotonic()
        deadline = started + 60
        children = [(plan, owned.start(plan['name'], plan['command'],
                    environment=plan['environment'], cwd=plan['cwd'])) for plan in self.plans]
        manifest.update(status='running', workload_started_monotonic=started,
                        pids={name: child.pid for name, child in owned.children}, executed_binaries={})
        released = self.args.kind != 'publishers'
        try:
            while True:
                owned.check_brokers()
                now = time.monotonic()
                if now >= deadline:
                    raise dev.RunError('workload exceeded its 60-second deadline')
                for plan, child in children:
                    name = plan['name']
                    if child.poll() is not None:
                        if child.returncode != 0:
                            raise dev.RunError(f'{name} exited with {child.returncode}')
                        continue
                    if name not in manifest['executed_binaries']:
                        declared = next(p for p in manifest['binaries'] if p.endswith('/throughput_test'))
                        try:
                            target = os.readlink(f'/proc/{child.pid}/exe')
                            if target != declared:
                                continue  # numactl has not exec'd the client yet.
                            digest = dev.binary_digest(Path(f'/proc/{child.pid}/exe'))
                        except FileNotFoundError:
                            continue
                        if digest != manifest['binaries'][declared]:
                            raise dev.RunError(name + ' executed binary differs from preflight')
                        manifest['executed_binaries'][name] = {'path': declared, 'sha256': digest, 'pid': child.pid}
                        snapshot = dev.placement_snapshot(child, hardware['nodes']['0']['cpus'], 0, run_dir, name)
                        if not snapshot['thread_cpu_masks']:
                            raise dev.RunError(name + ' placement was not observed')
                        manifest['placement'][name] = snapshot
                if not released and all((Path(p['cwd']) / 'ready').read_text() == '1\n'
                                        if (Path(p['cwd']) / 'ready').exists() else False for p, _ in children):
                    go_ns = time.time_ns() + 100_000_000
                    temporary = run_dir / 'publish-go-ns.tmp'
                    temporary.write_text(str(go_ns) + '\n')
                    temporary.replace(run_dir / 'publish-go-ns')
                    manifest['publish_go_wall_ns'] = go_ns
                    self.go_ns = go_ns
                    released = True
                self.check_final_artifacts(run_dir)
                if all(child.poll() is not None for _, child in children):
                    break
                dev.write_json(run_dir / 'manifest.json', manifest)
                time.sleep(0.01)
            if not released or len(manifest['executed_binaries']) != len(children):
                raise dev.RunError('clients did not prove executable identity and barrier completion')
        finally:
            manifest['workload_ended_monotonic'] = time.monotonic()

    def check_final_artifacts(self, run_dir):
        # Called while waiting and again by the shared lifecycle after broker
        # cleanup, which can itself emit final drain logs.
        if sum(p.stat().st_size for p in run_dir.glob('*.log')) > 64 * dev.MIB:
            raise dev.RunError('workload exceeded the 64MiB log budget')

    def validate(self, run_dir):
        try:
            return self._validate(run_dir)
        except (csv.Error, KeyError, TypeError, ValueError, OverflowError) as error:
            raise dev.RunError('malformed workload result: ' + str(error)) from error

    def _validate(self, run_dir):
        results = []
        for index, plan in enumerate(self.plans):
            directory = Path(plan['cwd'])
            log = (run_dir / (plan['name'] + '.log')).read_text(errors='replace')
            payload = self.args.payloads[index] * dev.MIB
            count = payload // dev.MESSAGE_BYTES
            result = ack_and_routing(log, count, self.args.allowlists[index])
            if self.args.kind == 'gap':
                audits = re.findall(r'\[ORDERED_DELIVERY_AUDIT\] status=passed messages=(\d+) expected=(\d+) payload_bytes=(\d+) duplicates=(\d+) parse_errors=(\d+) export_gaps=(\d+) indexed_payload=1\b', log)
                if audits != [(str(count), str(count), str(payload), '0', '0', '0')]:
                    raise dev.RunError('sender gap did not retain exact indexed payload delivery')
                start = re.findall(r'\[ORDER5_GAP_INJECT\] phase=start batch_seq=0 delay_ms=(\d+) wall_ms=(\d+)', log)
                end = re.findall(r'\[ORDER5_GAP_INJECT\] phase=end batch_seq=0 wall_ms=(\d+)', log)
                if len(start) != 1 or len(end) != 1 or int(start[0][0]) != self.args.gap_ms or int(end[0]) - int(start[0][1]) < self.args.gap_ms:
                    raise dev.RunError('one-shot sender delay was not observed completely')
                result['observed_gap_wall_ms'] = int(end[0]) - int(start[0][1])
            elif self.args.kind == 'latency':
                ordering = one_row(directory / 'delivery_ordering_assertion.csv')
                wanted = {'Target': count, 'Delivered': count, 'RecordedSamples': count, 'Pass': 1,
                    'FirstTotalOrder': 0, 'LastTotalOrder': count - 1, 'MinUid': 1, 'MaxUid': count}
                wanted.update({key: 0 for key in ('TimedOut', 'InvalidMessages', 'DuplicateTotalOrder',
                    'OutOfOrderTotalOrder', 'DuplicateUid', 'MissingUid', 'OutOfOrderPerClient')})
                if any(int(ordering[key]) != value for key, value in wanted.items()):
                    raise dev.RunError('strict latency delivery/UID/order counters did not pass')
                summary = one_row(directory / 'latency_benchmark_summary.csv')
                if (int(summary['message_count']) != count or int(summary['total_message_size_bytes']) != payload or
                        int(summary['message_size_bytes']) != dev.MESSAGE_BYTES or
                        float(summary['target_offered_load_mbps']) != self.args.rates[index] or
                        summary['steady_rate'] != str(self.args.steady_rate).lower()):
                    raise dev.RunError('latency summary differs from planned finite workload')
                finite(summary, ('achieved_offered_load_mbps', 'achieved_publish_goodput_mbps', 'achieved_e2e_goodput_mbps'))
                stats = one_row(directory / 'delivery_latency_stats.csv')
                if stats['Metric'] != 'publish_to_deliver_latency' or stats['Unit'] != 'us' or any(int(stats[k]) != count for k in ('Count', 'Parsed', 'Recorded')) or int(stats['Dropped']) != 0:
                    raise dev.RunError('delivery-latency sample population is incomplete')
                finite(stats, ('Median', 'p95', 'p99'))
                if not float(stats['Median']) <= float(stats['p95']) <= float(stats['p99']):
                    raise dev.RunError('delivery-latency quantiles are inconsistent')
                result.update(delivery_ordering=ordering, finite_latency_summary=summary, delivery_latency=stats,
                              fifo_checked_messages=int(ordering['FifoCheckedMessages']), payload_fifo_proven=False)
            else:
                starts = re.findall(r'Publisher push start \(wall ns\): (\d+)', log)
                announced = re.findall(r'Push-ready barrier: go_ns=(\d+)', log)
                if (self.go_ns is None or announced != [str(self.go_ns)] or len(starts) != 1 or
                        int(starts[0]) < self.go_ns or 'Push-ready barrier: go time reached' not in log):
                    raise dev.RunError('publisher did not reach the real client start barrier')
                result['publish_start_wall_ns'] = int(starts[0])
            results.append(result)
        if len({r['client_id'] for r in results}) != len(results):
            raise dev.RunError('independent publisher processes collided on session client identity')
        result = {'clients': results, 'scope': self.args.kind + ' finite workload; see manifest workload.validation_scope'}
        if self.args.kind == 'publishers':
            starts = [r['publish_start_wall_ns'] for r in results]
            result['observed_publish_start_spread_ns'] = max(starts) - min(starts)
            result['common_go_wall_ns'] = self.go_ns
        return result


def parse_args(argv=None):
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('kind', choices=('latency', 'gap', 'publishers'))
    cli.add_argument('--clients', type=int, choices=range(1, 5), default=None)
    cli.add_argument('--payload-mib', default='32', help='one value or comma-separated value per client; 1..32 each')
    cli.add_argument('--target-mibps', default=None, help='one or comma-separated rates; 1..4096, 0 means unpaced publishers')
    cli.add_argument('--client-brokers', default=None, help='semicolon-separated per-client broker lists, e.g. 0;0,1,2')
    cli.add_argument('--threads', type=int, choices=(1, 2), default=None, help='default 1; indexed gap requires 2')
    cli.add_argument('--steady-rate', action='store_true', help='also apply historical four-batch flush/1.5ms pause policy')
    cli.add_argument('--gap-ms', type=int, default=None, help='one sender delay at batch0; gap profile only, 1..10ms, default 1')
    # Explicit shared options: unknown/misspelled options fail before any cluster.
    cli.add_argument('--build-dir', type=Path, default=dev.ROOT / 'build/debug')
    cli.add_argument('--brokers', type=int, choices=(1, 3), default=1)
    cli.add_argument('--run-root', type=Path, default=Path('/tmp'))
    cli.add_argument('--dry-run', action='store_true')
    cli.add_argument('--automatic-mapping', action='store_true')
    cli.add_argument('--startup-timeout', type=float, default=120)
    cli.add_argument('--shutdown-timeout', type=float, default=15)
    args = cli.parse_args(argv)
    args.clients = args.clients or (2 if args.kind == 'publishers' else 1)
    args.threads = args.threads if args.threads is not None else (2 if args.kind == 'gap' else 1)
    if args.kind != 'publishers' and args.clients != 1:
        cli.error('latency and indexed gap validation support one publisher session')
    def values(text, lower, upper, cast):
        try:
            result = [cast(v) for v in text.split(',')]
            if len(result) == 1: result *= args.clients
            if len(result) != args.clients or any(not math.isfinite(v) or not lower <= v <= upper for v in result):
                raise ValueError()
            return result
        except (ValueError, OverflowError):
            cli.error(f'expected one or {args.clients} values in {lower}..{upper}')
    args.payloads = values(args.payload_mib, 1, 32, int)
    rate_text = args.target_mibps if args.target_mibps is not None else ('128' if args.kind == 'latency' else '0')
    args.rates = values(rate_text, 0 if args.kind != 'latency' else 1, 4096, float)
    if any(round(rate, 3) != rate for rate in args.rates):
        cli.error('rates support at most three fractional digits, matching the native summary precision')
    if any(rate and size * 1.04 / rate > 15 for size, rate in zip(args.payloads, args.rates)):
        cli.error('planned paced send must fit within 15 seconds of the 60-second client deadline')
    if args.gap_ms is not None and (not 1 <= args.gap_ms <= 10 or args.kind != 'gap'):
        cli.error('gap delay is 1..10ms and applies only to gap')
    args.gap_ms = args.gap_ms if args.gap_ms is not None else 1
    if args.kind == 'gap' and (args.target_mibps is not None or args.steady_rate):
        cli.error('the indexed E2E path does not implement offered-load pacing')
    if args.kind == 'gap' and args.threads != 2:
        cli.error('indexed gap requires two sender threads; --threads 1 would serialize the delayed path')
    try:
        args.allowlists = [list(range(args.brokers))] * args.clients if args.client_brokers is None else [list(map(int, part.split(','))) for part in args.client_brokers.split(';')]
        if len(args.allowlists) != args.clients or any(not v or len(v) != len(set(v)) or not set(v).issubset(range(args.brokers)) for v in args.allowlists): raise ValueError()
    except ValueError:
        cli.error('each client needs one nonempty unique broker allowlist within the configured cluster')
    if any(0 not in allowed for allowed in args.allowlists):
        cli.error('ORDER5 ACK1 requires broker 0 in every allowlist: the head-owned ACK/fence channel currently shares publish connections; follower-only routing is unsupported')
    return args


def main(argv=None):
    args = parse_args(argv)
    workload = Workload(args)
    common = ['--build-dir', str(args.build_dir), '--brokers', str(args.brokers), '--run-root', str(args.run_root),
              '--startup-timeout', str(args.startup_timeout), '--shutdown-timeout', str(args.shutdown_timeout)]
    if args.dry_run: common.append('--dry-run')
    if args.automatic_mapping: common.append('--automatic-mapping')
    return dev.main(common, profile=dev.SmokeProfile(name='dev-dram-workload-' + args.kind,
        audit_enabled=args.kind == 'gap', validator=workload.validate, workload=workload))


if __name__ == '__main__':
    sys.exit(main())
