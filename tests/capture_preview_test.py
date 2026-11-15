#!/usr/bin/env python3
"""Combined recording/preview fidelity, source mapping and isolated sink failure."""
import argparse
import copy
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import stock_preview
import stock_video
from stock_mov_test import calibration


class Combined(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='gpr-capture-preview-test-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.raws=[];self.metadata=[]
        for source in range(2):
            raw=self.root/f'{source}.raw'
            raw.write_bytes(struct.pack('<'+'H'*(128*96),*((x*(123+source*20)+y*97+x*y*(29+source*2)+(x^y)*37)&16383 for y in range(96) for x in range(128))))
            meta=calibration(15000-source*100)
            if source:
                import raw_media
                meta['image'].append(raw_media.record(274,3,1,struct.pack('<H',6)))
            path=self.root/f'{source}.json';path.write_text(json.dumps(meta));self.raws.append(raw);self.metadata.append(path)
        self.base=self.root/'base.gvid'
        self.capture(self.base,'--frames','24')
        self.profiles=self.root/'base-profiles'
        self.downsample=8
        stock_preview.prepare(self.base,self.profiles,args.compiler,8)
        self.expected_payloads={}
        with self.base.open('rb') as file:
            header=stock_video.read_header(file)
            for frame in stock_video.records(file,header):self.expected_payloads[frame['metadata']['source_index']]=frame['payload']

    def capture(self,output,*extra,preview=False,okay=True):
        command=[str(args.capture),'--output',str(output),'--width','128','--height','96','--quality','3',
                 '--workspace-mib','16','--submit-timeout-ms','10000']
        for raw,meta in zip(self.raws,self.metadata):command+=['--input',str(raw),'--source-metadata',str(meta)]
        if preview:
            for index in (0,1):command+=['--preview-profile',str(self.profiles/f'{index:06}.gprp')]
        result=subprocess.run(command+list(extra),capture_output=True,text=True,timeout=60)
        self.assertEqual(result.returncode==0,okay,result.stderr)
        return [json.loads(line) for line in result.stdout.splitlines()]

    def verify(self,output,events,every,failed=False):
        with output.open('rb') as file:
            header=stock_video.read_header(file);frames=list(stock_video.records(file,header))
        self.assertTrue(header['_end_marker_verified'])
        captures={row['index']:row for row in events if row['event']=='frame'}
        previews=[row for row in events if row['event']=='preview']
        summary=next(row for row in events if row['event']=='preview_summary')
        final=events[-1];self.assertEqual(final['event'],'summary');self.assertTrue(final['complete'])
        self.assertEqual(final['written'],len(frames));self.assertEqual(final['accepted']+final['overruns'],final['attempts'])
        self.assertLessEqual(summary['inflight_high_water'],2)
        self.assertEqual(summary['offered'],summary['rendered']+summary['discarded'])
        self.assertEqual(summary['rendered'],len(previews));self.assertEqual(summary['error']!=0,failed)
        directory=self.root/(output.stem+'-profiles');stock_preview.prepare(output,directory,args.compiler,self.downsample)
        playback=subprocess.run([str(args.player),'--input',str(output),'--profile-dir',str(directory)],capture_output=True,text=True,timeout=60,check=True)
        offline={row['index']:row for row in map(json.loads,playback.stdout.splitlines()) if row['event']=='preview'}
        previous=-1
        for row in previews:
            index=row['index'];self.assertGreater(index,previous);previous=index
            self.assertEqual(index%every,0)
            frame=frames[index];source=frame['metadata']['source_index']
            self.assertEqual(row['timestamp_ns'],frame['pts_ns']);self.assertEqual(row['source_index'],source)
            self.assertEqual(row['payload_crc32'],zlib.crc32(frame['payload']))
            self.assertEqual(row['rgb_crc32'],offline[index]['rgb_crc32'])
            self.assertEqual((row['width'],row['height']),(offline[index]['width'],offline[index]['height']))
            self.assertIn(index,captures)
        for frame in frames:self.assertEqual(frame['payload'],self.expected_payloads[frame['metadata']['source_index']])
        return final,summary,previews

    def test_copied_and_leased_capture_with_slow_sink(self):
        for leased in (False,True):
            with self.subTest(leased=leased):
                output=self.root/f'combined-{leased}.gvid'
                extra=['--frames','24','--preview-every','3','--preview-delay-ms','20']+(['--leased'] if leased else [])
                events=self.capture(output,*extra,preview=True)
                final,summary,previews=self.verify(output,events,3)
                self.assertEqual(final['accepted'],24);self.assertTrue(previews)
                self.assertEqual(summary['offered'],8)

    def test_overruns_keep_original_source_profile_association(self):
        output=self.root/'overruns.gvid'
        events=self.capture(output,'--frames','256','--queue','1','--submit-timeout-ms','0','--preview-every','1','--preview-delay-ms','20',preview=True)
        final,summary,previews=self.verify(output,events,1)
        self.assertGreater(final['overruns'],0);self.assertTrue(previews)

    def test_sink_failure_keeps_recording_complete_and_reports_failure(self):
        output=self.root/'failure.gvid'
        events=self.capture(output,'--frames','24','--preview-every','1','--preview-fail-after','1',preview=True,okay=False)
        final,summary,previews=self.verify(output,events,1,failed=True)
        self.assertEqual(final['written'],24);self.assertEqual(summary['error'],-7);self.assertEqual(len(previews),1)

    def test_encoder_wavelets_match_offline_and_isolate_sink_failure(self):
        self.downsample=4;self.profiles=self.root/'quarter-profiles'
        stock_preview.prepare(self.base,self.profiles,args.compiler,4)
        for failed in (False,True):
            output=self.root/f'encoder-{failed}.gvid'
            options=['--frames','24','--preview-every','1','--preview-from-encoder']
            if failed:options+=['--preview-fail-after','1']
            events=self.capture(output,*options,preview=True,okay=not failed)
            final,summary,previews=self.verify(output,events,1,failed=failed)
            self.assertEqual(final['written'],24)
            self.assertEqual(summary['pipeline'],'encoder_wavelets')
            self.assertGreater(summary['reconstruction_scratch_peak_bytes'],0)
            self.assertLessEqual(summary['reconstruction_scratch_peak_bytes'],summary['reconstruction_scratch_bytes'])
            self.assertLessEqual(summary['reconstruction_scratch_bytes'],3*64*1024*1024)
            self.assertTrue(previews)
            if failed:self.assertEqual(len(previews),1)

    def test_streamed_encoder_preview_matches_offline_and_isolates_failure(self):
        self.downsample=4;self.profiles=self.root/'streamed-quarter-profiles'
        stock_preview.prepare(self.base,self.profiles,args.compiler,4)
        for leased in (False,True):
            for failed in (False,True):
                with self.subTest(leased=leased,failed=failed):
                    output=self.root/f'streamed-{leased}-{failed}.gvid'
                    options=['--frames','24','--preview-every','1','--preview-streamed','--preview-sync-stages']
                    if leased:options+=['--leased']
                    if failed:options+=['--preview-fail-after','1']
                    events=self.capture(output,*options,preview=True,okay=not failed)
                    final,summary,previews=self.verify(output,events,1,failed=failed)
                    self.assertEqual(final['written'],24)
                    self.assertTrue(summary['streamed_inverse'])
                    self.assertEqual(summary['pipeline'],'encoder_wavelets')
                    self.assertGreater(summary['stream_buffer_bytes'],0)
                    self.assertLessEqual(summary['stream_buffer_bytes'],128*1024*1024)
                    self.assertEqual(summary['reconstruction_scratch_bytes'],0)
                    self.assertEqual(summary['replaced'],0)
                    self.assertEqual(len(previews),1 if failed else 24)
        output=self.root/'streamed-slow-display.gvid'
        events=self.capture(output,'--frames','24','--preview-every','1','--preview-streamed','--preview-delay-ms','20',preview=True)
        final,summary,previews=self.verify(output,events,1)
        self.assertEqual(final['written'],24)
        self.assertTrue(summary['streamed_inverse'])
        self.assertLessEqual(summary['inflight_high_water'],2)

    def test_encoder_wavelets_reject_nonquarter_profile_before_output(self):
        output=self.root/'invalid-encoder-profile.gvid'
        self.capture(output,'--frames','2','--preview-from-encoder',preview=True,okay=False)
        self.assertFalse(output.exists())
        streamed=self.root/'invalid-streamed-profile.gvid'
        self.capture(streamed,'--frames','2','--preview-streamed',preview=True,okay=False)
        self.assertFalse(streamed.exists())

    def test_serialized_compute_stages_preserve_frames_and_isolate_failure(self):
        self.downsample=4;self.profiles=self.root/'sync-quarter-profiles'
        stock_preview.prepare(self.base,self.profiles,args.compiler,4)
        for failed in (False,True):
            output=self.root/f'sync-stages-{failed}.gvid'
            options=['--frames','24','--preview-every','1','--preview-from-encoder','--preview-sync-stages']
            if failed:options+=['--preview-fail-after','1']
            events=self.capture(output,*options,preview=True,okay=not failed)
            final,summary,previews=self.verify(output,events,1,failed=failed)
            self.assertEqual(final['written'],24);self.assertTrue(summary['sync_compute_stages'])
            self.assertGreater(summary['producer_flush_wait_s'],0)
            self.assertEqual(summary['replaced'],0)
            self.assertEqual(len(previews),1 if failed else 24)
        output=self.root/'sync-without-preview.gvid'
        self.capture(output,'--preview-sync-stages',okay=False)
        self.assertFalse(output.exists())

    def test_writeback_policy_and_unchanged_recording(self):
        for options in (['--writeback-mib','1025'],['--writeback-mib','1','--fsync-each']):
            output=self.root/'invalid-writeback.gvid'
            self.capture(output,*options,okay=False)
            self.assertFalse(output.exists())
        output=self.root/'writeback.gvid'
        if not sys.platform.startswith('linux'):
            self.capture(output,'--writeback-mib','1',okay=False)
            self.assertFalse(output.exists())
            return
        events=self.capture(output,'--frames','128','--writeback-mib','1')
        final=events[-1]
        self.assertTrue(final['complete']);self.assertEqual(final['written'],128)
        self.assertEqual(final['writeback_mib'],1);self.assertFalse(final['fsync_each'])
        self.assertGreater(final['writeback_requests'],0)
        self.assertGreaterEqual(final['writeback_scheduled_bytes'],1024*1024)
        self.assertGreater(final['writeback_page_bytes'],0)
        self.assertEqual(final['writeback_scheduled_bytes']%final['writeback_page_bytes'],0)
        self.assertLessEqual(final['writeback_scheduled_bytes'],output.stat().st_size)
        with output.open('rb') as file:
            header=stock_video.read_header(file);frames=list(stock_video.records(file,header))
        self.assertTrue(header['_end_marker_verified']);self.assertEqual(len(frames),128)
        for frame in frames:
            self.assertEqual(frame['payload'],self.expected_payloads[frame['metadata']['source_index']])


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    for name in ('capture','compiler','player'):parser.add_argument('--'+name,type=Path,required=True)
    args,remaining=parser.parse_known_args();args=argparse.Namespace(**{k:v.resolve() for k,v in vars(args).items()})
    unittest.main(argv=[sys.argv[0]]+remaining)
