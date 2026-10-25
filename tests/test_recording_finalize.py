"""Real recording remux/publish safety. Sources are deterministic test media."""
from pathlib import Path
import hashlib,json,os,subprocess,sys,tempfile,unittest
import av

ROOT=Path(__file__).resolve().parents[1]
OUT=Path(os.environ.get('CGPLAY_RECORDING_FRONTEND_ARTIFACTS', ROOT/'tests/artifacts/migration_1_0_8_20261025/recording_frontend/finalizer'))
PACKAGE=ROOT/'build_win_full/package/full/CGPlay'
SCRIPT=ROOT/'tools/cgplay/export/recording_finalize.py'
from tests.test_recording_helper import Helper,media

class RecordingFinalizeTests(unittest.TestCase):
    def setUp(self):
        OUT.mkdir(parents=True,exist_ok=True)
        self.directory=Path(tempfile.mkdtemp(prefix=self._testMethodName+'-',dir=OUT))
        self.helper=Helper(self.directory)
        self.target=self.directory/'published.mp4'
        self.sessiondir=self.directory/('.cgplay-recording-'+self.helper.session)
        self.descriptor=self.sessiondir/'session.json'
        self.request_index=0

    def tearDown(self):
        if self.helper.process.poll() is None and not any(row.get('state')=='Preparing' for row in self.helper.events):
            self.helper.process.stdin.close()
        self.helper.close()

    def call(self,operation,observer=None,**changes):
        self.request_index+=1
        request=dict(schema=1,operation=operation,sessionId=self.helper.session,sessionDirectory=str(self.sessiondir),
                     sessionDescriptorPath=str(self.descriptor),finalOutputPath=str(self.target),protectedMediaPaths=[],
                     helperPlan=self.helper.plan(),ffmpeg=str(PACKAGE/'ffmpeg.exe'),ffprobe=str(PACKAGE/'ffprobe.exe'),helperExited=True)
        request.update(changes)
        path=self.directory/('request'+str(self.request_index)+'.json')
        path.write_text(json.dumps(request),encoding='utf-8')
        with (self.directory/('finalizer'+str(self.request_index)+'.stderr')).open('wb') as log:
            process=subprocess.Popen([sys.executable,'-B',str(SCRIPT),'--request',str(path)],stdout=subprocess.PIPE,stderr=log,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            messages=[]
            for line in process.stdout:
                value=json.loads(line);messages.append(value)
                if observer:observer(value,path)
            process.wait(timeout=30)
            process.stdout.close()
        (self.directory/('finalizer'+str(self.request_index)+'.json')).write_text(json.dumps(dict(exitCode=process.returncode,messages=messages),indent=2),encoding='utf-8')
        return process.returncode,messages[-1]

    def capture(self,**settings):
        code,value=self.call('prepare',helperPlan=self.helper.plan(**settings))
        self.assertEqual(0,code,value)
        self.helper.output=Path(value['mkvPath'])
        self.helper.start(**settings)
        import time;time.sleep(1.3)
        self.helper.stop()
        return self.helper.output

    def test_real_publish_and_explicit_recovery_keep_original_mkv(self):
        mkv=self.capture();before=hashlib.sha256(mkv.read_bytes()).hexdigest()
        code,value=self.call('finalize');self.assertEqual(0,code,value)
        self.assertGreater(media(self.target)['video'][0]['count'],20)
        self.assertEqual(before,hashlib.sha256(mkv.read_bytes()).hexdigest())
        recovered=self.directory/'recovered.mp4'
        code,value=self.call('recover',finalOutputPath=str(recovered));self.assertEqual(0,code,value)
        self.assertGreater(media(recovered)['video'][0]['count'],20)

    def test_native_dimensions_survive_real_publish_and_recovery(self):
        mkv=self.capture(nativeResolution=True,sourceWidth=321,sourceHeight=181,fps=120)
        before=hashlib.sha256(mkv.read_bytes()).hexdigest()
        descriptor=json.loads(self.descriptor.read_text())
        self.assertTrue(descriptor['nativeResolution']);self.assertIsNone(descriptor['width'])
        for operation,name in [('finalize','published.mp4'),('recover','native-recovered.mp4')]:
            target=self.directory/name
            code,value=self.call(operation,finalOutputPath=str(target));self.assertEqual(0,code,value)
            with av.open(str(target)) as container:
                stream=container.streams.video[0]
                self.assertEqual((324,182),(stream.width,stream.height));self.assertEqual(120,stream.base_rate)
                times=[float(frame.pts*frame.time_base) for frame in container.decode(video=0)]
                self.assertGreater(len(times),100)
                self.assertTrue(all(abs(b-a-1/120)<.002 for a,b in zip(times,times[1:])))
        self.assertEqual(before,hashlib.sha256(mkv.read_bytes()).hexdigest())

    def test_native_format_metadata_is_required_bounded_and_probed(self):
        mkv=self.capture(nativeResolution=True)
        sidecar=Path(str(mkv)+'.json');original=json.loads(sidecar.read_text())
        formats=[None,dict(nativeResolution=True,width=8192,height=8192,fps=30),
                 dict(nativeResolution=True,width=324,height=182,fps=121),
                 dict(nativeResolution=True,width=True,height=182,fps=30),
                 dict(nativeResolution=True,width=640,height=360,fps=30)]
        for video_format in formats:
            changed=dict(original)
            if video_format is None:changed.pop('videoFormat',None)
            else:changed['videoFormat']=video_format
            sidecar.write_text(json.dumps(changed),encoding='utf-8')
            code,value=self.call('recover');self.assertNotEqual(0,code,value);self.assertFalse(self.target.exists())
        sidecar.write_text(json.dumps(original),encoding='utf-8')
        self.assertTrue(mkv.exists())

    def test_killed_helper_mkv_recovers_through_real_finalizer(self):
        code,value=self.call('prepare');self.assertEqual(0,code,value)
        self.helper.output=Path(value['mkvPath']);self.helper.start()
        import time;time.sleep(2.7)
        self.helper.process.kill();self.helper.finish()
        mkv=self.helper.output;before=hashlib.sha256(mkv.read_bytes()).hexdigest()
        code,value=self.call('recover');self.assertEqual(0,code,value)
        self.assertGreater(media(self.target)['video'][0]['count'],25)
        self.assertEqual(before,hashlib.sha256(mkv.read_bytes()).hexdigest())

    def test_all_sequence_members_hardlinks_and_old_output_are_preserved(self):
        first=self.directory/'render.1001.exr';last=self.directory/'render.1002.exr'
        first.write_bytes(b'first original');last.write_bytes(b'unselected original')
        os.link(last,self.target)
        before=hashlib.sha256(last.read_bytes()).hexdigest()
        code,value=self.call('prepare',protectedMediaPaths=[str(first)])
        self.assertNotEqual(0,code)
        self.assertEqual(before,hashlib.sha256(last.read_bytes()).hexdigest())
        self.assertEqual(b'unselected original',self.target.read_bytes())

    def test_read_only_media_through_compatibility_junction_is_protected(self):
        source=self.directory/'render.1001.exr';source.write_bytes(b'original sequence')
        alias=Path('C:/Users/1/Desktop/RVLite')/source.relative_to(ROOT)
        if not alias.exists():self.skipTest('Repository compatibility junction unavailable')
        code,value=self.call('prepare',protectedMediaPaths=[str(alias)])
        self.assertEqual(0,code,value)
        descriptor=json.loads(self.descriptor.read_text())
        self.assertEqual(os.path.normcase(os.path.realpath(source)),descriptor['protectedInputs'][0]['path'])
        self.assertEqual(b'original sequence',source.read_bytes())

    def test_target_created_during_probe_is_not_overwritten(self):
        mkv=self.capture()
        def race(value,path):
            if value.get('phase')=='verify':self.target.write_bytes(b'concurrent original')
        code,value=self.call('finalize',observer=race)
        self.assertNotEqual(0,code,value)
        self.assertEqual(b'concurrent original',self.target.read_bytes())
        self.assertTrue(mkv.is_file())
        self.assertTrue(list(self.directory.glob('.cgplay-publish-*/complete.mp4')))

    def test_helper_exit_cancel_and_identity_mismatch_fail_without_commit(self):
        mkv=self.capture()
        code,value=self.call('finalize',helperExited=False);self.assertNotEqual(0,code);self.assertFalse(self.target.exists())
        def cancel(value,path):
            if value.get('phase')=='remux':path.with_suffix('.cancel').write_bytes(b'cancel')
        code,value=self.call('finalize',observer=cancel);self.assertNotEqual(0,code);self.assertFalse(self.target.exists())
        content=mkv.read_bytes();mkv.unlink();mkv.write_bytes(content)
        code,value=self.call('recover');self.assertNotEqual(0,code);self.assertIn('identity-changed',value['error'])
        self.assertFalse(self.target.exists())

    def test_untrusted_descriptor_cannot_read_or_delete_arbitrary_paths(self):
        mkv=self.capture();original=self.directory/'unrelated.txt';original.write_bytes(b'never delete')
        value=json.loads(self.descriptor.read_text());value['mkvFile']='../../unrelated.txt';self.descriptor.write_text(json.dumps(value))
        code,value=self.call('recover');self.assertNotEqual(0,code)
        self.assertEqual(b'never delete',original.read_bytes());self.assertTrue(mkv.exists());self.assertFalse(self.target.exists())

    def test_packet_stream_above_old_128mib_limit_uses_incremental_statistics(self):
        import importlib.util
        specification=importlib.util.spec_from_file_location('recording_finalize_test_module',SCRIPT)
        module=importlib.util.module_from_spec(specification);specification.loader.exec_module(module)
        fixture=self.directory/'packet_fixture.py'
        fixture.write_text("import sys\nfor i in range(750000):\n sys.stdout.write(f'stream_index=0|pts={i}|dts={i}|duration=1|size=1000|padding='+('x'*160)+'\\n')\n",encoding='utf-8')
        result=module.scan_packets([sys.executable,'-B',str(fixture)],self.directory,self.directory/'never.cancel',timeout=120)
        self.assertGreater(result['rawBytes'],128*1024*1024)
        self.assertEqual(750000,result['counts'][0]);self.assertEqual(749999,result['lastDts'][0]);self.assertEqual(32,result['queueLimit'])
        self.assertFalse((self.directory/'packets.stdout').exists())
        (self.directory/'streaming_probe.json').write_text(json.dumps(result,indent=2),encoding='utf-8')

    def test_oversize_session_metadata_fails_before_capture_or_descriptor_write(self):
        import importlib.util
        specification=importlib.util.spec_from_file_location('recording_finalize_limit_test',SCRIPT)
        module=importlib.util.module_from_spec(specification);specification.loader.exec_module(module)
        module.MAX_DESCRIPTOR=1024
        for frame in range(1001,1021):(self.directory/f'render.{frame}.exr').write_bytes(b'original')
        request=dict(sessionId=self.helper.session,sessionDirectory=str(self.sessiondir),finalOutputPath=str(self.target),
                     helperPlan=self.helper.plan(),protectedMediaPaths=[str(self.directory/'render.1001.exr')])
        with self.assertRaisesRegex(ValueError,'metadata-size-limit-before-capture'):module.prepare(request)
        self.assertFalse(self.descriptor.exists());self.assertFalse(self.target.exists())
        self.assertTrue(all(path.read_bytes()==b'original' for path in self.directory.glob('*.exr')))
        (self.directory/'metadata_limit.json').write_text(json.dumps(dict(testLimit=1024,sourceMembers=20,rejectedBeforeCapture=True)),encoding='utf-8')

if __name__=='__main__':unittest.main()
