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

RAINBOW_STOPS = ((0.00, '#0000ff'), (0.20, '#00bfff'), (0.40, '#00ff00'),
                 (0.60, '#ffff00'), (0.80, '#ff7f00'), (1.00, '#ff0000'))

def rainbow(value: float, low: float, high: float) -> str:
    """Cats2d-inspired rainbow: slow blue, fast red; no external asset needed."""
    t = 0.5 if high == low else max(0.0, min(1.0, (value - low) / (high - low)))
    for (left, a), (right, b) in zip(RAINBOW_STOPS, RAINBOW_STOPS[1:]):
        if t <= right:
            q = (t - left) / (right - left)
            ar, ag, ab = (int(a[i:i+2], 16) for i in (1, 3, 5))
            br, bg, bb = (int(b[i:i+2], 16) for i in (1, 3, 5))
            return f'rgb({round(ar+(br-ar)*q)},{round(ag+(bg-ag)*q)},{round(ab+(bb-ab)*q)})'
    return RAINBOW_STOPS[-1][1]

def svg_defs() -> list[str]:
    stops = ''.join(f'<stop offset="{at*100:.0f}%" stop-color="{color}"/>' for at, color in RAINBOW_STOPS)
    return [f'<defs><linearGradient id="throughput-rainbow" x1="0" x2="1" y1="0" y2="0">{stops}</linearGradient><pattern id="self-pair" width="8" height="8" patternUnits="userSpaceOnUse" patternTransform="rotate(45)"><rect width="8" height="8" fill="#dbe3ee"/><line x1="0" y1="0" x2="0" y2="8" stroke="#94a3b8" stroke-width="3"/></pattern></defs>']

def legend(lines: list[str], x: float, y: float, width: float, low: float, high: float, label='Median throughput (M items/s)'):
    lines += [f'<text x="{x}" y="{y-8}" class="legend-label">{label}</text>',
              f'<rect x="{x}" y="{y}" width="{width}" height="16" rx="2" fill="url(#throughput-rainbow)" stroke="#475569"/>']
    for fraction in (0, .25, .5, .75, 1):
        xx=x+width*fraction; value=low+(high-low)*fraction
        lines += [f'<line x1="{xx}" y1="{y+16}" x2="{xx}" y2="{y+21}" stroke="#334155"/>', f'<text x="{xx}" y="{y+35}" text-anchor="middle" class="tick">{value:.0f}</text>']
    lines += [f'<text x="{x}" y="{y+51}" class="note">Slowest = blue · fastest = red · linear scale for this view</text>']

def svg_start(width, height, title, subtitle):
    return [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-label="{title}">',
            '<style>text{font-family:ui-sans-serif,system-ui,sans-serif;fill:#172033}.title{font-size:22px;font-weight:700}.subtitle{font-size:12px;fill:#475569}.axis{font-size:11px;font-weight:600}.tick{font-size:10px;fill:#475569}.note{font-size:10px;fill:#64748b}.legend-label{font-size:11px;font-weight:600}</style>',
            '<rect width="100%" height="100%" fill="#ffffff"/>', *svg_defs(),
            f'<text x="28" y="32" class="title">{title}</text>', f'<text x="28" y="53" class="subtitle">{subtitle}</text>']

def svg_heatmap(rows, width: int, path: Path, meta: dict):
    cells = {(int(r['producer_cpu']), int(r['consumer_cpu'])): float(r['median_mps']) for r in rows if int(r['width']) == width}
    cpus = sorted(set(x for pair in cells for x in pair)); n=len(cpus); values=list(cells.values()); low=min(values, default=0); high=max(values, default=1)
    cell=max(3, min(16, 920/max(n, 1))); plot=cell*n; left=105; top=112; right=48; bottom=118; W=left+plot+right; H=top+plot+bottom
    mode='Scalar API' if width == 0 else f'Fixed batch width {width}'
    lines=svg_start(W,H,f'{mode} — directed CPU-pair throughput', f'Rows: producer CPU · columns: consumer CPU · each cell: median M items/s. {meta.get("placement_confidence", "")}.')
    lines += [f'<text x="{left+plot/2}" y="{top-42}" text-anchor="middle" class="axis">Consumer logical CPU →</text>', f'<text x="22" y="{top+plot/2}" transform="rotate(-90 22 {top+plot/2})" text-anchor="middle" class="axis">Producer logical CPU →</text>']
    label_step=max(1, (n+23)//24)
    for i,cpu in enumerate(cpus):
        pos=left+(i+.5)*cell
        if i % label_step == 0: lines.append(f'<text x="{pos}" y="{top-8}" text-anchor="middle" class="tick">{cpu}</text>')
        pos=top+(i+.5)*cell
        if i % label_step == 0: lines.append(f'<text x="{left-7}" y="{pos+3}" text-anchor="end" class="tick">{cpu}</text>')
    for i,p in enumerate(cpus):
        for j,c in enumerate(cpus):
            x=left+j*cell; y=top+i*cell; v=cells.get((p,c))
            if p == c:
                lines.append(f'<rect x="{x}" y="{y}" width="{cell+.1}" height="{cell+.1}" fill="url(#self-pair)"><title>CPU {p} → itself: excluded by benchmark design</title></rect>')
            elif v is None:
                lines.append(f'<rect x="{x}" y="{y}" width="{cell+.1}" height="{cell+.1}" fill="#e2e8f0"><title>CPU {p} → CPU {c}: missing measurement</title></rect>')
            else:
                lines.append(f'<rect x="{x}" y="{y}" width="{cell+.1}" height="{cell+.1}" fill="{rainbow(v,low,high)}"><title>Producer CPU {p} → consumer CPU {c}: median {v:.3f} M items/s</title></rect>')
    legend(lines,left,H-74,min(plot,420),low,high)
    lines += [f'<rect x="{left+min(plot,420)+28}" y="{H-72}" width="14" height="14" fill="url(#self-pair)"/><text x="{left+min(plot,420)+48}" y="{H-60}" class="note">self pair excluded</text>', '</svg>']
    path.write_text('\n'.join(lines))

def cube_bins(cpus, count):
    count=min(count, len(cpus)); groups=[]
    for i in range(count):
        lo=round(i*len(cpus)/count); hi=round((i+1)*len(cpus)/count)
        groups.append(cpus[lo:hi])
    return groups

def svg_voxel_cube(rows, path: Path, meta: dict, max_cpus: int):
    """Static isometric volumetric heat cube. X=producer, Y=consumer, Z=mode."""
    cpus=sorted({int(r['producer_cpu']) for r in rows}|{int(r['consumer_cpu']) for r in rows}); widths=sorted({int(r['width']) for r in rows})
    bins=cube_bins(cpus, max_cpus or 12); n=len(bins); raw={(int(r['producer_cpu']),int(r['consumer_cpu']),int(r['width'])):float(r['median_mps']) for r in rows}
    volume={}
    for z,w in enumerate(widths):
        for x,gp in enumerate(bins):
            for y,gc in enumerate(bins):
                vals=[raw[(p,c,w)] for p in gp for c in gc if (p,c,w) in raw]
                if vals: volume[x,y,z]=median(vals)
    vals=list(volume.values()); low=min(vals,default=0); high=max(vals,default=1)
    cw=max(14,min(33,360/max(n,1))); ch=cw*.46; dz=max(22, min(52, 120/max(len(widths),1))); ox=390; oy=145+len(widths)*dz; W=900; H=max(590,oy+n*ch+140)
    def pt(x,y,z): return ox+(x-y)*cw, oy+(x+y)*ch-z*dz
    lines=svg_start(W,H,'Volumetric topology heat cube',f'X = producer CPU group · Y = consumer CPU group · Z = API/batch mode · color = median throughput. {meta.get("placement_confidence", "")}')
    lines += ['<text x="28" y="76" class="note">Cube cells are median aggregates when CPU count exceeds display limit. Exact directed paths: linked CSV and 2D heatmaps. Self-pairs excluded.</text>']
    # Draw back-to-front, then top/right/front faces for visible solid cube cells.
    for z,w in enumerate(widths):
        for x in range(n):
            for y in range(n-1,-1,-1):
                v=volume.get((x,y,z))
                if v is None: continue
                a=pt(x,y,z); b=pt(x+1,y,z); d=pt(x,y+1,z); e=pt(x+1,y+1,z); depth=dz
                col=rainbow(v,low,high)
                top=f'{a[0]},{a[1]} {b[0]},{b[1]} {e[0]},{e[1]} {d[0]},{d[1]}'
                right=f'{b[0]},{b[1]} {e[0]},{e[1]} {e[0]},{e[1]+depth} {b[0]},{b[1]+depth}'
                front=f'{d[0]},{d[1]} {e[0]},{e[1]} {e[0]},{e[1]+depth} {d[0]},{d[1]+depth}'
                lines += [f'<g><polygon points="{right}" fill="{col}" opacity=".63"/><polygon points="{front}" fill="{col}" opacity=".82"/><polygon points="{top}" fill="{col}" stroke="#ffffff" stroke-width=".35"/><title>Producer group {bins[x][0]}–{bins[x][-1]} → consumer group {bins[y][0]}–{bins[y][-1]}; {"scalar" if w==0 else "width "+str(w)}; median {v:.3f} M items/s</title></g>']
    # Dimension axes and labels.
    for i,g in enumerate(bins):
        label=str(g[0]) if len(g)==1 else f'{g[0]}–{g[-1]}'
        x,y=pt(i+.5,0,0); lines.append(f'<text x="{x}" y="{y+19}" text-anchor="middle" class="tick">{label}</text>')
        x,y=pt(0,i+.5,0); lines.append(f'<text x="{x-9}" y="{y+5}" text-anchor="end" class="tick">{label}</text>')
    for z,w in enumerate(widths):
        x,y=pt(0,n,z); lines.append(f'<text x="{x-10}" y="{y+5}" text-anchor="end" class="tick">{"scalar" if w==0 else "width "+str(w)}</text>')
    lines += [f'<text x="{ox+cw*n+20}" y="{oy+ch*n/2}" class="axis">Producer CPU group →</text>', f'<text x="{ox-cw*n-185}" y="{oy+ch*n/2}" class="axis">← Consumer CPU group</text>', f'<text x="{ox-cw*n-135}" y="{oy-ch*n/2-10}" class="axis">Mode / width ↑</text>']
    legend(lines,28,H-90,260,low,high)
    lines.append('</svg>'); path.write_text('\n'.join(lines))

def svg_depth(rows, path: Path, meta: dict):
    widths=sorted({int(r['width']) for r in rows}); by={w:[float(r['median_mps']) for r in rows if int(r['width'])==w] for w in widths}; low=min((min(v) for v in by.values() if v),default=0); high=max((max(v) for v in by.values() if v),default=1)
    W,H,L,R,T,B=900,520,90,45,95,115; pw=W-L-R; ph=H-T-B
    lines=svg_start(W,H,'Batch mode comparison — topology distribution',f'Point: median across directed CPU pairs. Vertical whisker: min–max pair median. {meta.get("placement_confidence", "")}')
    def y(v): return T+ph*(1-(v-low)/(high-low or 1))
    lines += [f'<line x1="{L}" y1="{H-B}" x2="{W-R}" y2="{H-B}" stroke="#334155"/><line x1="{L}" y1="{T}" x2="{L}" y2="{H-B}" stroke="#334155"/>']
    for frac in range(5):
        val=low+(high-low)*frac/4; yy=y(val); lines += [f'<line x1="{L}" y1="{yy}" x2="{W-R}" y2="{yy}" stroke="#e2e8f0"/><text x="{L-9}" y="{yy+4}" text-anchor="end" class="tick">{val:.0f}</text>']
    for i,w in enumerate(widths):
        vs=by[w]; x=L+pw*(i+.5)/max(len(widths),1); med=median(vs); label='scalar' if w==0 else str(w)
        lines += [f'<line x1="{x}" y1="{y(min(vs))}" x2="{x}" y2="{y(max(vs))}" stroke="#64748b" stroke-width="2"/><line x1="{x-7}" y1="{y(min(vs))}" x2="{x+7}" y2="{y(min(vs))}" stroke="#64748b"/><line x1="{x-7}" y1="{y(max(vs))}" x2="{x+7}" y2="{y(max(vs))}" stroke="#64748b"/><circle cx="{x}" cy="{y(med)}" r="7" fill="{rainbow(med,low,high)}" stroke="#172033"><title>{label}: pair median {med:.3f}; range {min(vs):.3f}–{max(vs):.3f} M items/s</title></circle>', f'<text x="{x}" y="{H-B+22}" text-anchor="middle" class="tick">{label}</text>']
    lines += [f'<text x="{L}" y="{T-18}" class="axis">M items/s</text>', f'<text x="{L+pw/2}" y="{H-26}" text-anchor="middle" class="axis">API mode / fixed batch width</text>']
    legend(lines, W-305, 72, 250, low, high, 'Point color: pair median (M items/s)')
    lines += ['</svg>']
    path.write_text('\n'.join(lines))

def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--out',type=Path,default=ROOT/'docs'/'topology-matrix'); p.add_argument('--max-cpus',type=int,default=0); p.add_argument('--3d-max-cpus',type=int,default=12,help='CPU groups shown per cube dimension; 0 uses 12 groups'); p.add_argument('--transfers',type=int,default=2162160,help='calibration transfers; exact multiple of all fixed widths'); p.add_argument('--min-sample-ms',type=int,default=0,help='calibrate every producer/to/width cell to at least this timed duration; 0 disables'); p.add_argument('--rounds',type=int,default=3); p.add_argument('--widths',default='',help='comma-separated widths; 0=scalar, empty=all supported widths'); p.add_argument('--warmups',type=int,default=1); p.add_argument('--producer-shard',type=int,default=0,help='zero-based producer-row shard'); p.add_argument('--producer-shards',type=int,default=1,help='total non-overlapping producer-row shards'); p.add_argument('--no-build',action='store_true'); p.add_argument('--render-only',action='store_true',help='regenerate SVGs from existing results.csv and metadata.json'); a=p.parse_args()
    a.out.mkdir(parents=True,exist_ok=True)
    csv_path=a.out/'results.csv'
    if a.render_only:
        if not csv_path.exists(): raise SystemExit(f'--render-only needs {csv_path}')
        meta=json.loads((a.out/'metadata.json').read_text()) if (a.out/'metadata.json').exists() else metadata()
        rows=aggregate(load(csv_path)); (a.out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
        max_width=max(map(lambda r:int(r['width']),rows),default=0); svg_heatmap(rows,0,a.out/'scalar-heatmap.svg',meta); svg_heatmap(rows,max_width,a.out/f'fixed-{max_width}-heatmap.svg',meta); svg_depth(rows,a.out/'width-depth.svg',meta); svg_voxel_cube(rows,a.out/'topology-3d-heatmap.svg',meta,a.__dict__['3d_max_cpus'])
        print(f'Rendered SVG artifacts from {csv_path}')
        return
    meta=metadata(); (a.out/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
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
