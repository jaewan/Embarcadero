#!/usr/bin/env python3
"""Bounded workload adapter checks; only tiny local fixture processes are used."""
import csv
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import dev_cluster as dev
import experiment_workload as work


def log_for(count=256, identity=71, broker=0):
    return (f'[ACK_VERIFY] normalized_received={count} raw_received={count-1} target={count} 100%\n'
            f'[ORDER5_ROUTING] client_id={identity} retransmit_attempts=0 session_fenced_observed=0 '
            f'session_rto_min_ms=60000 broker{broker}_msgs={count}\n')


def csv_row(path, row):
    with path.open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(row))
        writer.writeheader()
        writer.writerow(row)


class WorkloadTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)

    def workload(self, *args):
        workload = work.Workload(work.parse_args(list(args)))
        workload.commands([], Path('/fixture/throughput_test'), self.directory/'config.yaml', self.directory)
        return workload

    def test_per_client_plan_preserves_payload_rate_routing_and_isolates_outputs(self):
        item = self.workload('publishers', '--brokers', '3', '--clients', '2', '--payload-mib', '1,2',
                             '--target-mibps', '16,32', '--client-brokers', '0;0,1,2')
        self.assertEqual(item.args.payloads, [1,2])
        self.assertEqual(item.args.rates, [16,32])
        for index, plan in enumerate(item.plans):
            self.assertIn(str((index+1)*dev.MIB), plan['command'])
            self.assertEqual(plan['environment']['EMBARCADERO_PUBLISH_BROKER_ALLOWLIST'], ('0','0,1,2')[index])
            self.assertEqual(Path(plan['environment']['EMBARCADERO_PUSH_READY_FILE']).parent, Path(plan['cwd']))
            self.assertTrue(plan['environment']['EMBARCADERO_DATA_DIR'].startswith(plan['cwd']))
        self.assertNotEqual(item.plans[0]['cwd'], item.plans[1]['cwd'])
        self.assertEqual(item.plans[0]['environment']['EMBARCADERO_PUSH_GO_FILE'], item.plans[1]['environment']['EMBARCADERO_PUSH_GO_FILE'])

    def test_invalid_scope_rate_and_route_rejected_before_runner(self):
        for args in [('latency','--clients','2'), ('gap','--target-mibps','10'),
                     ('latency','--target-mibps','nan'), ('latency','--target-mibps','1'),
                     ('latency','--target-mibps','128.1234'),
                     ('publishers','--clients','2','--client-brokers','0'),
                     ('publishers','--client-brokers','0;4'), ('gap','--gap-ms','11'),
                     ('gap','--threads','1'), ('latency','--gap-ms','1'), ('publishers','--target-mibps',''),
                     ('latency','--message-bytes','15'), ('gap','--message-bytes','7'),
                     ('publishers','--message-bytes','0'), ('publishers','--message-bytes','1048577'),
                     ('latency','--message-count','0'), ('latency','--message-count','262145'),
                     ('latency','--message-count','256','--payload-mib','1'),
                     ('publishers','--message-count','40000','--message-bytes','1024')]:
            with self.subTest(args=args), self.assertRaises(SystemExit), mock.patch('sys.stderr'):
                work.parse_args(list(args))

    def test_exact_small_message_plan_and_sub_mib_accounting(self):
        item = self.workload('latency', '--message-bytes', '16', '--message-count', '256')
        self.assertEqual(item.args.payload_bytes, [4096])
        self.assertEqual(item.args.message_counts, [256])
        self.assertEqual(item.plans[0]['command'][item.plans[0]['command'].index('-m') + 1], '16')
        self.assertEqual(item.plans[0]['command'][item.plans[0]['command'].index('-s') + 1], '4096')
        config=dev.effective_config(1); env={}; selected={}
        manifest={'memory':{'cgroup_limits':[]},'limitations':[]}
        item.prepare(config,env,selected,manifest,{'nodes':{'0':{'free_bytes':3*dev.GIB}}},mock.Mock(brokers=1),self.directory)
        self.assertEqual(manifest['application_payload_bytes'],4096)
        self.assertEqual(manifest['workload']['message_count_per_client'],[256])

    def test_physical_cxl_option_reaches_owned_runner(self):
        with mock.patch.object(dev, 'main', return_value=0) as lifecycle:
            self.assertEqual(work.main(['gap', '--physical-cxl']), 0)
        self.assertIn('--physical-cxl', lifecycle.call_args.args[0])

    def test_follower_only_ack_route_rejected_before_owned_lifecycle(self):
        for kind, routes in [('latency', '1,2'), ('gap', '1'), ('publishers', '0;1,2')]:
            with self.subTest(kind=kind), mock.patch.object(dev, 'main') as lifecycle, mock.patch('sys.stderr'):
                with self.assertRaises(SystemExit) as error:
                    work.main([kind, '--brokers', '3', '--client-brokers', routes])
                self.assertEqual(error.exception.code, 2)
                lifecycle.assert_not_called()

    def test_ack_uses_authoritative_frontier_and_rejects_wrong_route(self):
        self.assertEqual(work.ack_and_routing(log_for(),256,[0])['raw_ack_diagnostic'],255)
        shorter = log_for().replace('session_rto_min_ms=60000', 'session_rto_min_ms=10000')
        self.assertEqual(work.ack_and_routing(shorter,256,[0],10000)['messages'],256)
        with self.assertRaises(dev.RunError): work.ack_and_routing(shorter,256,[0],60000)
        for log in (log_for().replace('normalized_received=256','normalized_received=255'),
                    log_for(broker=1), log_for().replace('retransmit_attempts=0','retransmit_attempts=1')):
            with self.assertRaises(dev.RunError): work.ack_and_routing(log,256,[0])

    def test_gap_requires_complete_actual_injection_and_indexed_delivery(self):
        item = self.workload('gap','--payload-mib','1','--gap-ms','2')
        log=log_for()+'[ORDERED_DELIVERY_AUDIT] status=passed messages=256 expected=256 payload_bytes=1048576 duplicates=0 parse_errors=0 export_gaps=0 indexed_payload=1\n'
        path=self.directory/'client-0.log';path.write_text(log)
        with self.assertRaises(dev.RunError):item.validate(self.directory)
        path.write_text(log+'[ORDER5_GAP_INJECT] phase=start batch_seq=0 delay_ms=2 wall_ms=100\n[ORDER5_GAP_INJECT] phase=end batch_seq=0 wall_ms=102\n')
        self.assertEqual(item.validate(self.directory)['clients'][0]['observed_gap_wall_ms'],2)

    def test_latency_rejects_partial_sample_population_or_soft_order_failure(self):
        item=self.workload('latency','--payload-mib','1')
        (self.directory/'client-0.log').write_text(log_for())
        folder=self.directory/'client-0'
        ordering={key:0 for key in ('TimedOut','InvalidMessages','DuplicateTotalOrder','OutOfOrderTotalOrder','DuplicateUid','MissingUid','OutOfOrderPerClient')}
        ordering.update(Target=256,Delivered=256,RecordedSamples=256,Pass=1,FirstTotalOrder=0,LastTotalOrder=255,MinUid=1,MaxUid=256,FifoCheckedMessages=0)
        summary=dict(message_count=256,total_message_size_bytes=dev.MIB,message_size_bytes=4096,target_offered_load_mbps=128,steady_rate='false',achieved_offered_load_mbps=120,achieved_publish_goodput_mbps=110,achieved_e2e_goodput_mbps=100)
        stats=dict(Metric='publish_to_deliver_latency',Unit='us',Count=256,Parsed=256,Recorded=256,Dropped=0,Median=2,p95=3,p99=4)
        csv_row(folder/'delivery_ordering_assertion.csv',ordering)
        csv_row(folder/'latency_benchmark_summary.csv',summary)
        csv_row(folder/'delivery_latency_stats.csv',stats)
        self.assertEqual(item.validate(self.directory)['clients'][0]['messages'],256)
        stats['Recorded']=255;csv_row(folder/'delivery_latency_stats.csv',stats)
        with self.assertRaises(dev.RunError):item.validate(self.directory)
        stats['Recorded']=256;csv_row(folder/'delivery_latency_stats.csv',stats)
        ordering['DuplicateUid']=1;csv_row(folder/'delivery_ordering_assertion.csv',ordering)
        with self.assertRaises(dev.RunError):item.validate(self.directory)

    def test_duplicate_process_client_identity_is_not_qualified(self):
        item=self.workload('publishers','--payload-mib','1')
        item.go_ns=100
        for index in range(2):
            (self.directory/f'client-{index}.log').write_text(log_for()+'Push-ready barrier: go_ns=100\nPublisher push start (wall ns): 100\nPush-ready barrier: go time reached\n')
        with self.assertRaisesRegex(dev.RunError,'collided'):item.validate(self.directory)

    def test_distinct_publishers_qualify_exact_per_client_work_and_record_start_spread(self):
        item=self.workload('publishers','--brokers','3','--payload-mib','1,2','--client-brokers','0;0,1,2')
        item.go_ns=100
        for index,count,identity,broker,start in ((0,256,71,0,110),(1,512,72,2,130)):
            (self.directory/f'client-{index}.log').write_text(log_for(count,identity,broker)+
                f'Push-ready barrier: go_ns=100\nPublisher push start (wall ns): {start}\nPush-ready barrier: go time reached\n')
        result=item.validate(self.directory)
        self.assertEqual([r['messages'] for r in result['clients']],[256,512])
        self.assertEqual(result['observed_publish_start_spread_ns'],20)

    def test_wrong_or_early_go_timestamp_cannot_qualify_publishers(self):
        item=self.workload('publishers','--clients','1','--payload-mib','1');item.go_ns=100
        for announced,start in ((99,100),(100,99)):
            (self.directory/'client-0.log').write_text(log_for()+f'Push-ready barrier: go_ns={announced}\nPublisher push start (wall ns): {start}\nPush-ready barrier: go time reached\n')
            with self.assertRaisesRegex(dev.RunError,'barrier'):item.validate(self.directory)

    def test_malformed_csv_is_a_concise_owned_runner_error(self):
        item=self.workload('latency','--payload-mib','1')
        (self.directory/'client-0.log').write_text(log_for())
        csv_row(self.directory/'client-0'/'delivery_ordering_assertion.csv',{'wrong':'header'})
        with self.assertRaisesRegex(dev.RunError,'malformed workload result'):item.validate(self.directory)

    def test_profile_memory_budget_includes_each_pool_and_all_payloads(self):
        item=work.Workload(work.parse_args(['publishers','--clients','4']))
        config=dev.effective_config(3);env={};selected={};manifest={'memory':{'cgroup_limits':[]},'limitations':[]}
        item.prepare(config,env,selected,manifest,{'nodes':{'0':{'free_bytes':12*dev.GIB}}},mock.Mock(brokers=3),self.directory)
        self.assertEqual(manifest['application_payload_bytes'],128*dev.MIB)
        self.assertEqual(manifest['memory']['additional_memory_budget_bytes'],82*dev.GIB)
        self.assertLess(manifest['initial_payload_reservation_upper_bound_bytes'],dev.SEGMENT_BYTES)
        self.assertEqual(selected['EMBARCADERO_LATENCY_ACK_PRIMARY'],'0')
        with self.assertRaises(dev.RunError):
            item.prepare(config,env,selected,manifest,{'nodes':{'0':{'free_bytes':11*dev.GIB}}},mock.Mock(brokers=3),self.directory)

    def test_fast_exited_client_cannot_bypass_final_log_budget(self):
        item=self.workload('publishers','--clients','1','--payload-mib','1')
        child=mock.Mock(pid=100,returncode=0);child.poll.return_value=0
        owned=mock.Mock(children=[('client-0',child)]);owned.start.return_value=child
        manifest={'binaries':{'/fixture/throughput_test':'expected'},'placement':{}}
        # Sparse file creates no 64MiB allocation or broker workload.
        with (self.directory/'overflow.log').open('wb') as stream:stream.truncate(64*dev.MIB+1)
        with self.assertRaisesRegex(dev.RunError,'log budget'):
            item.execute(owned,manifest,{'nodes':{'0':{'cpus':[96]}}},self.directory)

    def test_real_fixture_children_reach_barrier_before_shared_release_and_use_owned_cleanup(self):
        item=self.workload('publishers','--payload-mib','1')
        program='''import os,time,signal,sys
from pathlib import Path
signal.signal(signal.SIGTERM,lambda *_:sys.exit(0))
Path('identity.json').write_text(str(os.getpid()))
Path(os.environ['EMBARCADERO_PUSH_READY_FILE']).write_text('1\\n')
go=Path(os.environ['EMBARCADERO_PUSH_GO_FILE'])
while not go.exists():time.sleep(.002)
Path('observed-go').write_text(go.read_text())
time.sleep(.05)
'''
        for plan in item.plans:plan['command']=[sys.executable,'-c',program]
        manifest={'binaries':{'/fixture/throughput_test':'expected'},'placement':{}}
        owned=dev.OwnedProcesses(self.directory,dict(os.environ),2)
        try:
            with mock.patch.object(work.os,'readlink',return_value='/fixture/throughput_test'), mock.patch.object(dev,'binary_digest',return_value='expected'), mock.patch.object(dev,'placement_snapshot',return_value={'thread_cpu_masks':{'1':'96'},'shared_mapping_pages':{}}):
                item.execute(owned,manifest,{'nodes':{'0':{'cpus':[96]}}},self.directory)
            self.assertEqual(len(manifest['executed_binaries']),2)
            observed=[(Path(p['cwd'])/'observed-go').read_text() for p in item.plans]
            self.assertEqual(observed,[str(manifest['publish_go_wall_ns'])+'\n']*2)
        finally:owned.close()
        self.assertFalse(owned.forced)
        self.assertTrue(all(child.returncode==0 for _,child in owned.children))


if __name__=='__main__':unittest.main()
