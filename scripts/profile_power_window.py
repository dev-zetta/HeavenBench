"""Explicit read-only observer state for one ordinary comparison window."""
import json
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

R=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(R/'scripts'))
import profile_compare as pc
from profile_harness.monitor import file_identity
from profile_harness.power_observer import validate_csv
from profile_harness.process import _identity,_signal_owned,_parent_death_signal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cohort', type=Path)
    parser.add_argument('label')
    args = parser.parse_args()
    cohort = args.cohort.resolve(strict=True)
    label = args.label
    with pc.store_open(cohort) as store:
        _,protocol=pc.record(store,'protocol')
        pc.require(label in protocol['spec']['order'], 'undeclared window label')
        expected=protocol['spec']['power_observer']
    pc.require(Path(expected['path']).name == 'xpu-smi', 'unsupported observer tool')
    pc.require(file_identity(expected['path'])==expected['identity'], 'observer tool identity changed')
    out=cohort/'observers'/label;out.mkdir(parents=True,exist_ok=False)
    argv=[expected['path'],'--id='+protocol['spec']['pci'],'--query-gpu=power.draw,clocks.current.graphics,clocks.current.media,clocks.throttle.reason,memory.used','--loop=2','--count=200','--file='+str(out/'power.csv')]
    parent=_identity(os.getpid())
    pc.require(parent is not None, 'observer parent identity unavailable')
    with (out/'stdout.log').open('xb') as so,(out/'stderr.log').open('xb') as se:
        child=subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=so,stderr=se,close_fds=True,start_new_session=True,preexec_fn=lambda:_parent_death_signal(parent,signal.SIGTERM))
        identity=_identity(child.pid)
        pc.require(identity is not None, 'observer child identity unavailable')
        pc.write_new(out/'launch.json',{'argv':argv,'identity':identity,'started_monotonic_ns':time.monotonic_ns(),'tool':expected['identity']})
        try:
            pc.run(cohort,label)
            alive=child.poll() is None
        finally:
            _signal_owned(identity,signal.SIGTERM)
            child.wait(timeout=5)
    with pc.store_open(cohort) as store:
        window_ref, window = pc.record(store, 'windows/'+label)
    coverage = validate_csv((out/'power.csv').read_text(), window['process']['elapsed_seconds']-4)
    pc.require(alive, 'observer exited before native completion')
    pc.require(file_identity(expected['path'])==expected['identity'], 'observer tool identity changed')
    pc.write_new(out/'receipt.json',{'passed':True,'tool_unchanged':True,'alive_after_window':alive,'coverage':coverage,'native_window_ref':window_ref,'tool':expected['identity'],'exit_code':child.returncode,'csv':file_identity(out/'power.csv'),'scope':'Observer state controlled identically for A/B. Read-only polling itself measurably affects performance; absolute FPS is conditional on this state. No device-setting changes.'})

    with pc.store_open(cohort) as store:
        receipt=json.loads((out/'receipt.json').read_text())
        pc.seal(store,'power_observers/'+label,'power-observer',receipt,[window_ref,store.put_file(out/'power.csv'),store.put_file(out/'launch.json'),store.put_file(__file__),store.put_file(expected['path'])])


if __name__=='__main__':main()
