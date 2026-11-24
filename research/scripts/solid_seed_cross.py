# SPDX-License-Identifier: GPL-3.0-or-later
"""Small deterministic cross-platform archives for the new solid profile."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool',type=Path,required=True)
    parser.add_argument('--work',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    args.work.mkdir(exist_ok=False)
    args.output.mkdir(exist_ok=False)
    initial,added,all_files=[args.work/x for x in ['initial','added','all']]
    for directory in [initial,added,all_files]:
        directory.mkdir()
    for i in range(16):
        rel=f'{["源代码","docs","new"][i%3]}/文件-{i}.txt'
        content=(f'function_{i}: alpha beta gamma 中文\n'*100).encode('utf-8') if i!=0 else b''
        for directory in [initial if i<8 else added,all_files]:
            p=directory/rel
            p.parent.mkdir(exist_ok=True)
            p.write_bytes(content)
    artifacts=[]
    def call(argv):
        proc=subprocess.run([str(args.tool),*map(str,argv)],capture_output=True,text=True,encoding='utf-8',timeout=120)
        if proc.returncode:
            raise RuntimeError(proc.stderr)
        return json.loads(proc.stdout)
    for plan in ['0x0','0x6','0xb','0x1f']:
        for mode in ['seed-solid','explicit-solid']:
            base=args.output/f'{mode}-{plan}-initial.sadx'
            call(['pack',mode,plan,initial,base,'off'])
            call(['verify',base,initial])
            artifacts.append(dict(name=base.name,bytes=base.stat().st_size,sha256=sha(base)))
            for how in ['segment','regroup','new-generation']:
                out=args.output/f'{mode}-{plan}-{how}.sadx'
                argv=['append',base,added,out]
                argv+=['--plan','0x6'] if how=='new-generation' else [how]
                call(argv)
                call(['verify',out,all_files])
                artifacts.append(dict(name=out.name,bytes=out.stat().st_size,sha256=sha(out)))
    (args.output/'archives.json').write_text(json.dumps(artifacts,indent=2),encoding='utf-8')
    print(json.dumps(dict(archives=len(artifacts),byte_exact=True)))


if __name__=='__main__':
    main()
