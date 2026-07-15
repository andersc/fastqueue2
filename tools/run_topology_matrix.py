#!/usr/bin/env python3
"""Build, run, summarize and plot FastQueue topology matrices.

Known queue backends are x86_64 and arm64. Linux pins each worker to logical
CPUs in its current cpuset. macOS labels results advisory because its public
thread-affinity API uses tags rather than hard logical-CPU pinning.
"""
from __future__ import annotations
import argparse, csv, json, os, platform, shutil, subprocess, sys
from collections import defaultdict
from pathlib import Path
from statistics import median

ROOT = Path(__file__).resolve().parents[1]

def command(args, **kwargs):
    print('+', ' '.join(map(str, args)))
    return subprocess.run(args, check=True, text=True, **kwargs)

def metadata():
    osname = platform.system()
    data = {"os": osname, "release": platform.release(), "machine": platform.machine(),
            "processor": platform.processor(), "python": sys.version.split()[0],
            "placement_confidence": "hard Linux logical-CPU pinning" if osname == "Linux" else "advisory macOS affinity; no hard logical-CPU pinning"}
    if osname == "Linux":
        data["allowed_cpus"] = sorted(os.sched_getaffinity(0))
        cpuinfo = Path('/proc/cpuinfo')
        if cpuinfo.exists():
            for line in cpuinfo.read_text(errors='replace').splitlines():
                if line.lower().startswith('model name'):
                    data['cpu_model'] = line.split(':', 1)[1].strip(); break
    elif osname == "Darwin":
        for key in ('machdep.cpu.brand_string', 'hw.physicalcpu', 'hw.logicalcpu', 'hw.cachelinesize'):
            p = subprocess.run(['sysctl', '-n', key], text=True, capture_output=True)
            if p.returncode == 0: data[key] = p.stdout.strip()
    return data

def build(build_dir: Path):
    compiler = shutil.which('clang++') or shutil.which('g++') or shutil.which('c++')
    if not compiler: raise SystemExit('No C++ compiler found')
    command(['cmake', '-S', ROOT, '-B', build_dir, '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_CXX_COMPILER={compiler}'])
    command(['cmake', '--build', build_dir, '--target', 'fast_queue_topology_matrix', '-j'])
    return build_dir / 'fast_queue_topology_matrix'

def load(path: Path):
    with path.open(newline='') as f: return list(csv.DictReader(f))

def aggregate(rows):
    groups = defaultdict(list); first = {}
    for r in rows:
        key = (int(r['producer_cpu']), int(r['consumer_cpu']), int(r['width']))
        groups[key].append(float(r['throughput_mps'])); first[key] = r
    out=[]
    for k, values in groups.items():
        r=dict(first[k]); r['median_mps']=median(values); r['sample_count']=len(values); out.append(r)
    return out

def svg_heatmap(rows, width: int, path: Path, meta: dict):
    cells={(int(r['producer_cpu']),int(r['consumer_cpu'])):float(r['median_mps']) for r in rows if int(r['width'])==width}
    cpus=sorted(set(x for k in cells for x in k)); n=len(cpus); size=max(28, min(60, 720//max(n,1))); left=150; top=100; plot=size*n
    values=list(cells.values()); lo=min(values,default=0); hi=max(values,default=1)
    def color(v):
        t=0 if hi==lo else (v-lo)/(hi-lo); return f'rgb({int(22+220*t)},{int(35+150*t)},{int(55+35*(1-t))})'
    lines=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{left+plot+30}" height="{top+plot+70}" viewBox="0 0 {left+plot+30} {top+plot+70}">', '<style>text{font:12px sans-serif;fill:#172033}.small{font-size:10px}.title{font-size:20px;font-weight:bold}</style>', '<rect width="100%" height="100%" fill="#f8fafc"/>', f'<text x="20" y="30" class="title">FastQueue topology matrix — {"Scalar API" if width==0 else "Fixed batch "+str(width)}</text>', f'<text x="20" y="52" class="small">Color: relative median throughput (M items/s). {meta["placement_confidence"]}.</text>', '<text x="20" y="75" class="small">Rows: producer logical CPU. Columns: consumer logical CPU. Diagonal intentionally blank.</text>']
    for i,c in enumerate(cpus):
        x=left+i*size; y=top+i*size; lines += [f'<text x="{x+size/2}" y="{top-8}" text-anchor="middle" class="small">{c}</text>', f'<text x="{left-8}" y="{y+size/2+3}" text-anchor="end" class="small">{c}</text>']
    for i,p in enumerate(cpus):
      for j,c in enumerate(cpus):
        v=cells.get((p,c)); x=left+j*size; y=top+i*size
        if v is None: lines.append(f'<rect x="{x}" y="{y}" width="{size-1}" height="{size-1}" fill="#cbd5e1"/>')
        else: lines += [f'<rect x="{x}" y="{y}" width="{size-1}" height="{size-1}" fill="{color(v)}"/>', f'<title>P{p} → C{c}: {v:.2f} M/s</title>']
    lines += [f'<text x="{left}" y="{top+plot+30}" class="small">min {lo:.1f} M/s</text>', f'<text x="{left+plot}" y="{top+plot+30}" text-anchor="end" class="small">max {hi:.1f} M/s</text>', '</svg>']
    path.write_text('\n'.join(lines))

def svg_depth(rows, path: Path, meta: dict):
    by=defaultdict(list)
    for r in rows: by[int(r['width'])].append(float(r['median_mps']))
    widths=sorted(by); med={w:median(by[w]) for w in widths}; hi=max(med.values(),default=1); W=900; H=440; L=70; B=65; pw=W-L-30; ph=H-B-60
    lines=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">','<style>text{font:13px sans-serif;fill:#172033}.title{font-size:20px;font-weight:bold}.small{font-size:11px}</style>','<rect width="100%" height="100%" fill="#f8fafc"/>','<text x="20" y="30" class="title">Batch-width depth — median across all ordered CPU pairs</text>',f'<text x="20" y="52" class="small">{meta["placement_confidence"]}. Error bars omitted: inspect CSV for every pair and round.</text>',f'<line x1="{L}" y1="{H-B}" x2="{W-30}" y2="{H-B}" stroke="#334155"/><line x1="{L}" y1="{H-B}" x2="{L}" y2="60" stroke="#334155"/>']
    for i,w in enumerate(widths):
        x=L+(i+.5)*pw/max(len(widths),1); h=ph*med[w]/hi; fill='#14b8a6' if w==0 else '#2563eb'; label='Scalar' if w==0 else str(w)
        lines += [f'<rect x="{x-14}" y="{H-B-h}" width="28" height="{h}" rx="3" fill="{fill}"/><title>{label}: {med[w]:.2f} M/s median</title>',f'<text x="{x}" y="{H-B+20}" text-anchor="middle" class="small">{label}</text>',f'<text x="{x}" y="{H-B-h-5}" text-anchor="middle" class="small">{med[w]:.0f}</text>']
    lines += [f'<text x="{L}" y="{H-18}" class="small">Scalar API is not a batch width. Fixed widths begin at 1.</text>', f'<text x="{L-8}" y="68" text-anchor="end" class="small">{hi:.0f} M/s</text>','</svg>']; path.write_text('\n'.join(lines))

def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--out',type=Path,default=ROOT/'docs'/'topology-matrix'); p.add_argument('--max-cpus',type=int,default=0); p.add_argument('--transfers',type=int,default=2162160); p.add_argument('--rounds',type=int,default=3); p.add_argument('--warmups',type=int,default=1); p.add_argument('--no-build',action='store_true'); a=p.parse_args()
    a.out.mkdir(parents=True,exist_ok=True); meta=metadata(); (a.out/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
    exe=(ROOT/'cmake-build-release'/'fast_queue_topology_matrix') if a.no_build else build(ROOT/'cmake-build-topology')
    csv_path=a.out/'results.csv'; command([exe,'--output',csv_path,'--max-cpus',str(a.max_cpus),'--transfers',str(a.transfers),'--rounds',str(a.rounds),'--warmups',str(a.warmups)])
    rows=aggregate(load(csv_path)); (a.out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
    max_width=max(map(lambda r:int(r['width']),rows),default=0); svg_heatmap(rows,0,a.out/'scalar-heatmap.svg',meta); svg_heatmap(rows,max_width,a.out/f'fixed-{max_width}-heatmap.svg',meta); svg_depth(rows,a.out/'width-depth.svg',meta)
    print(f'Wrote {csv_path}, summary.json, scalar-heatmap.svg, fixed-{max_width}-heatmap.svg, width-depth.svg')
if __name__ == '__main__': main()
