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
    cmake = shutil.which('cmake')
    if cmake:
        command([cmake, '-S', ROOT, '-B', build_dir, '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_CXX_COMPILER={compiler}'])
        command([cmake, '--build', build_dir, '--target', 'fast_queue_topology_matrix', '-j'])
    else:
        build_dir.mkdir(parents=True, exist_ok=True)
        command([compiler, '-std=c++20', '-O3', '-DNDEBUG', '-pthread', '-I', ROOT,
                 ROOT/'FastQueueTopologyMatrix.cpp', '-o', build_dir/'fast_queue_topology_matrix'])
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

def svg_voxel_cube(rows, path: Path, meta: dict, max_cpus: int):
    """Render throughput as color in an exploded isometric CPU×CPU×width cube.

    x = producer CPU, y = consumer CPU, z = scalar/fixed batch mode.  SVG uses
    an isometric projection: x runs down-right, y runs down-left, z stacks up.
    A linked SVG remains useful without JavaScript: every depth slice is visible.
    """
    cpus = sorted({int(r['producer_cpu']) for r in rows} | {int(r['consumer_cpu']) for r in rows})
    shown = cpus[:max_cpus] if max_cpus else cpus
    widths = sorted({int(r['width']) for r in rows})
    value = {(int(r['producer_cpu']), int(r['consumer_cpu']), int(r['width'])): float(r['median_mps']) for r in rows}
    visible = [v for (p, c, _), v in value.items() if p in shown and c in shown]
    lo, hi = min(visible, default=0.0), max(visible, default=1.0)
    n = len(shown); cw = max(9, min(24, 280 // max(n, 1))); ch = max(5, cw // 2)
    layer = max(ch * n + 8, 42); ox = 70 + cw * n; oy = 115 + layer * (len(widths) - 1)
    W = max(900, ox + cw * n + 150); H = oy + ch * n + 110
    def rgb(v, shade=1.0):
        t = 0 if hi == lo else (v - lo) / (hi - lo)
        # Viridis-like, stable global scale across every width slice.
        r, g, b = int(33 + 220*t), int(35 + 170*t), int(75 + 65*(1-t))
        return f'rgb({int(r*shade)},{int(g*shade)},{int(b*shade)})'
    def point(producer, consumer, depth): return (ox + (producer-consumer)*cw, oy + (producer+consumer)*ch - depth*layer)
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
        '<style>text{font:12px sans-serif;fill:#e2e8f0}.small{font-size:10px}.title{font-size:20px;font-weight:bold}.hint{font-size:11px;fill:#94a3b8}.slice{transition:opacity .15s}.slice:hover{opacity:1!important}.legend{cursor:pointer}.legend:hover{fill:#fff}</style>',
        '<rect width="100%" height="100%" fill="#07111f"/>',
        '<text x="24" y="30" class="title">FastQueue 3D topology heat map</text>',
        '<text x="24" y="51" class="hint">X: producer logical CPU · Y: consumer logical CPU · Z/depth: scalar or fixed batch width · color: median M items/s (one global scale)</text>',
        '<text x="24" y="69" class="hint">Exploded layers prevent occlusion. Hover voxel for exact path; click legend mode to isolate depth slice.</text>',
    ]
    if len(shown) < len(cpus):
        lines.append(f'<text x="24" y="87" class="hint">Overview limited to first {len(shown)} of {len(cpus)} logical CPUs. Re-run with --3d-max-cpus 0 for all CPUs.</text>')
    # Back-to-front slices and cells. Each datum is a shallow isometric voxel.
    for yi, width in enumerate(widths):
        label = 'Scalar API' if width == 0 else f'Fixed {width}'
        lines.append(f'<g class="slice" id="slice-{width}">')
        for zi, p in enumerate(shown):
            for xi, c in enumerate(shown):
                if p == c: continue
                v = value.get((p, c, width))
                if v is None: continue
                a = point(zi, xi, yi); b = point(zi+1, xi, yi); d = point(zi, xi+1, yi); e = point(zi+1, xi+1, yi)
                # top, right and front faces: throughput color carries fourth dimension.
                top = f'{a[0]},{a[1]} {b[0]},{b[1]} {e[0]},{e[1]} {d[0]},{d[1]}'
                right = f'{b[0]},{b[1]} {e[0]},{e[1]} {e[0]},{e[1]+4} {b[0]},{b[1]+4}'
                front = f'{d[0]},{d[1]} {e[0]},{e[1]} {e[0]},{e[1]+4} {d[0]},{d[1]+4}'
                lines += [f'<g><polygon points="{right}" fill="{rgb(v,.58)}"/><polygon points="{front}" fill="{rgb(v,.76)}"/><polygon points="{top}" fill="{rgb(v)}" stroke="#0f172a" stroke-width=".4"/><title>{label}: producer CPU {p} → consumer CPU {c}; {v:.3f} M items/s</title></g>']
        # Layer marker sits left of each plane.
        lx, ly = point(0, n, yi)
        lines.append(f'<text x="{lx-10}" y="{ly+4}" text-anchor="end" class="small">{label}</text></g>')
    # Axes and CPU labels on bottom plane.
    for i, cpu in enumerate(shown):
        x, y = point(i, 0, 0); lines.append(f'<text x="{x+cw/2}" y="{y+16}" text-anchor="middle" class="small">P{cpu}</text>')
        x, y = point(0, i, 0); lines.append(f'<text x="{x-cw/2-4}" y="{y+5}" text-anchor="end" class="small">C{cpu}</text>')
    lines += [
        f'<text x="24" y="{H-52}" class="hint">Global color scale: {lo:.1f} M/s <tspan fill="#94a3b8">(dark)</tspan> → {hi:.1f} M/s <tspan fill="#94a3b8">(bright)</tspan>. {meta["placement_confidence"]}.</text>',
        f'<text x="24" y="{H-32}" class="hint">Diagonal omitted: producer and consumer must be distinct. 2D per-mode heatmaps remain precise comparison view.</text>',
        '<script><![CDATA[function isolate(id){document.querySelectorAll(".slice").forEach(function(s){s.style.opacity=(s.id===id?"1":".10")})}]]></script>',
    ]
    # SVG script support varies by viewer; static exploded cube remains full fallback.
    for i, width in enumerate(widths):
        label = 'Scalar' if width == 0 else f'Fixed {width}'
        lines.append(f'<text x="{24+i*58}" y="96" class="small legend" onclick="isolate(\'slice-{width}\')">{label}</text>')
    lines.append('</svg>')
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
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--out',type=Path,default=ROOT/'docs'/'topology-matrix'); p.add_argument('--max-cpus',type=int,default=0); p.add_argument('--3d-max-cpus',type=int,default=16,help='logical CPUs shown in 3D overview; 0 shows all'); p.add_argument('--transfers',type=int,default=2162160,help='calibration transfers; exact multiple of all fixed widths'); p.add_argument('--min-sample-ms',type=int,default=0,help='calibrate every producer/to/width cell to at least this timed duration; 0 disables'); p.add_argument('--rounds',type=int,default=3); p.add_argument('--widths',default='',help='comma-separated widths; 0=scalar, empty=all supported widths'); p.add_argument('--warmups',type=int,default=1); p.add_argument('--producer-shard',type=int,default=0,help='zero-based producer-row shard'); p.add_argument('--producer-shards',type=int,default=1,help='total non-overlapping producer-row shards'); p.add_argument('--no-build',action='store_true'); a=p.parse_args()
    a.out.mkdir(parents=True,exist_ok=True); meta=metadata(); (a.out/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
    if a.no_build:
        candidates = [ROOT/'cmake-build-release'/'fast_queue_topology_matrix', ROOT/'cmake-build-topology'/'fast_queue_topology_matrix']
        exe = next((candidate for candidate in candidates if candidate.exists()), None)
        if exe is None:
            raise SystemExit('--no-build needs fast_queue_topology_matrix in cmake-build-release or cmake-build-topology')
    else:
        exe = build(ROOT/'cmake-build-topology')
    csv_path=a.out/'results.csv'; run_args=[exe,'--output',csv_path,'--max-cpus',str(a.max_cpus),'--transfers',str(a.transfers),'--rounds',str(a.rounds),'--warmups',str(a.warmups),'--min-sample-ms',str(a.min_sample_ms),'--producer-shard',str(a.producer_shard),'--producer-shards',str(a.producer_shards)] + (['--widths',a.widths] if a.widths else []); command(run_args); meta['benchmark']={'calibration_transfers':a.transfers,'min_sample_ms':a.min_sample_ms,'rounds':a.rounds,'warmups':a.warmups,'producer_shard':a.producer_shard,'producer_shards':a.producer_shards,'widths':a.widths or 'all supported'}; (a.out/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
    rows=aggregate(load(csv_path)); (a.out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
    max_width=max(map(lambda r:int(r['width']),rows),default=0); svg_heatmap(rows,0,a.out/'scalar-heatmap.svg',meta); svg_heatmap(rows,max_width,a.out/f'fixed-{max_width}-heatmap.svg',meta); svg_depth(rows,a.out/'width-depth.svg',meta); svg_voxel_cube(rows,a.out/'topology-3d-heatmap.svg',meta,a.__dict__['3d_max_cpus'])
    print(f'Wrote {csv_path}, summary.json, scalar-heatmap.svg, fixed-{max_width}-heatmap.svg, width-depth.svg, topology-3d-heatmap.svg')
if __name__ == '__main__': main()
