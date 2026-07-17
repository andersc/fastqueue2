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
    """Return only deterministic per-directed-pair median records."""
    groups = defaultdict(list)
    pins = defaultdict(list)
    for r in rows:
        key = (int(r['producer_cpu']), int(r['consumer_cpu']), int(r['width']))
        groups[key].append(float(r['throughput_mps']))
        if 'pinned' in r:
            pins[key].append(int(r['pinned']))
    return [
        {'producer_cpu': producer, 'consumer_cpu': consumer, 'width': width,
         'median_mps': median(values), 'sample_count': len(values),
         'all_pinned': all(pins[(producer, consumer, width)]) if pins[(producer, consumer, width)] else None}
        for (producer, consumer, width), values in sorted(groups.items())
    ]

# Reference-inspired throughput scale. Blue is slow; red is fast.
RAINBOW_STOPS = ((0.00, '#0000ff'), (0.20, '#00bfff'), (0.40, '#00ff00'),
                 (0.60, '#ffff00'), (0.80, '#ff7f00'), (1.00, '#ff0000'))

def raster_modules():
    import matplotlib
    matplotlib.use('Agg', force=True)
    import matplotlib.pyplot as plt
    from matplotlib.colors import LinearSegmentedColormap, ListedColormap
    import numpy as np
    return plt, np, LinearSegmentedColormap.from_list('fq_rainbow', [c for _, c in RAINBOW_STOPS])

def cpus_for(rows, meta):
    declared = meta.get('allowed_cpus', [])
    measured = {int(r['producer_cpu']) for r in rows} | {int(r['consumer_cpu']) for r in rows}
    # Retain declared CPU universe when metadata has it; otherwise exact measured IDs.
    return sorted(set(map(int, declared)) | measured)

def image_scale(values):
    low, high = min(values, default=0.0), max(values, default=1.0)
    return (low, high if high > low else low + 1.0)

def mode_label(width): return 'Scalar API' if width == 0 else f'Fixed batch width {width}'

def png_heatmap(rows, width: int, path: Path, meta: dict):
    plt, np, cmap = raster_modules()
    cpus = cpus_for(rows, meta); n = len(cpus); pos = {cpu: i for i, cpu in enumerate(cpus)}
    cells = {(int(r['producer_cpu']), int(r['consumer_cpu'])): float(r['median_mps'])
             for r in rows if int(r['width']) == width}
    low, high = image_scale(list(cells.values()))
    data = np.full((n, n), np.nan)
    for (producer, consumer), value in cells.items(): data[pos[producer], pos[consumer]] = value
    cmap.set_bad('#d9e1ea')  # missing measurement
    side = max(9.5, min(16.0, 5.5 + n / 24))
    fig, ax = plt.subplots(figsize=(side + 2.8, side), layout='constrained')
    im = ax.imshow(data, cmap=cmap, vmin=low, vmax=high, interpolation='nearest', aspect='equal')
    # Excluded self-pairs: explicit dark hatch, distinct from missing light gray.
    for i in range(n):
        ax.add_patch(plt.Rectangle((i-.5, i-.5), 1, 1, facecolor='#d4dbe5', edgecolor='#64748b', hatch='///', linewidth=.18))
    step = max(1, (n + 15) // 16); ticks = list(range(0, n, step))
    ax.set_xticks(ticks, [str(cpus[i]) for i in ticks], rotation=0, fontsize=8)
    ax.set_yticks(ticks, [str(cpus[i]) for i in ticks], fontsize=8)
    ax.set_xlabel('Consumer logical CPU')
    ax.set_ylabel('Producer logical CPU')
    ax.set_title(f'{mode_label(width)} — directed producer → consumer throughput', weight='bold', pad=14)
    ax.text(.5, 1.01, f'Cell = median M items/s; scale local to this image. {meta.get("placement_confidence", "")}',
            transform=ax.transAxes, ha='center', va='bottom', fontsize=8)
    cb = fig.colorbar(im, ax=ax, shrink=.83, pad=.025)
    cb.set_label('Median throughput (M items/s)\nblue = slow · red = fast', fontsize=9)
    ax.legend([plt.Rectangle((0, 0), 1, 1, facecolor='#d4dbe5', edgecolor='#64748b', hatch='///'),
               plt.Rectangle((0, 0), 1, 1, facecolor='#d9e1ea', edgecolor='#94a3b8')],
              ['self pair excluded', 'missing measurement'], loc='upper left', bbox_to_anchor=(1.02, .86), fontsize=8)
    fig.savefig(path, dpi=180, facecolor='white')
    plt.close(fig)

def cpu_bins(cpus, count):
    count = min(max(1, count), len(cpus)); return [cpus[round(i*len(cpus)/count):round((i+1)*len(cpus)/count)] for i in range(count)]

def bin_label(group):
    return str(group[0]) if len(group) == 1 else f'{group[0]}–{group[-1]}'

def png_voxel_cube(rows, path: Path, meta: dict, max_cpus: int):
    """Rasterized true 3D voxel volume; values aggregate exact pair medians per display bin."""
    plt, np, cmap = raster_modules()
    cpus = cpus_for(rows, meta); widths = sorted({int(r['width']) for r in rows})
    bins = cpu_bins(cpus, max_cpus or 10); n, zcount = len(bins), len(widths)
    raw = {(int(r['producer_cpu']), int(r['consumer_cpu']), int(r['width'])): float(r['median_mps']) for r in rows}
    volume = np.full((n, n, zcount), np.nan); coverage = []
    for x, producers in enumerate(bins):
        for y, consumers in enumerate(bins):
            for z, width in enumerate(widths):
                expected = len(producers) * len(consumers) - len(set(producers) & set(consumers))
                values = [raw[p, c, width] for p in producers for c in consumers if (p, c, width) in raw]
                coverage.append({'producer_display_bin': producers, 'consumer_display_bin': consumers,
                                 'width': width, 'expected_pairs': expected, 'measured_pairs': len(values),
                                 'excluded_self_pairs': len(set(producers) & set(consumers)),
                                 'median_of_pair_medians_mps': median(values) if values else None})
                if values: volume[x, y, z] = median(values)
    path.with_name('topology-voxel-cube-coverage.json').write_text(json.dumps({
        'aggregation': 'Each voxel is median of measured directed-pair medians in explicit display bins.',
        'display_bins': [list(b) for b in bins], 'widths': widths, 'voxels': coverage}, indent=2)+'\n')
    values = volume[~np.isnan(volume)]; low, high = image_scale(values)
    filled = ~np.isnan(volume); colors = cmap(np.clip((np.nan_to_num(volume, nan=low)-low)/(high-low), 0, 1))
    # Alpha stays opaque: color still maps exactly to numeric legend. Missing voxels shown wireframe.
    fig = plt.figure(figsize=(15, 12)); fig.subplots_adjust(left=.02, right=.82, bottom=.09, top=.90); ax = fig.add_subplot(projection='3d')
    ax.voxels(filled, facecolors=colors, edgecolor=(.15, .18, .24, .30), linewidth=.32, shade=True)
    # Dashed bounding frame makes cube depth legible even for a scalar-only one-layer volume.
    ax.set_box_aspect((1, 1, max(.55, zcount / n * 2.2)))
    ax.view_init(elev=26, azim=-52)
    tickstep=max(1, (n+9)//10); tickidx=list(range(0, n, tickstep))
    ax.set_xticks([i+.5 for i in tickidx], [bin_label(bins[i]) for i in tickidx], fontsize=8)
    ax.set_yticks([i+.5 for i in tickidx], [bin_label(bins[i]) for i in tickidx], fontsize=8)
    ax.set_zticks([i+.5 for i in range(zcount)], ['scalar' if w == 0 else f'width {w}' for w in widths], fontsize=8)
    ax.set_xlabel('Producer CPU display bin', labelpad=13); ax.set_ylabel('Consumer CPU display bin', labelpad=13); ax.set_zlabel('API / batch mode', labelpad=9)
    ax.set_title('Topology throughput — 3D voxel cube', weight='bold', pad=24)
    ax.text2D(.5, .965, 'Each cube = median of directed-pair medians in display-bin × display-bin × mode. Blue = slow; red = fast.', transform=ax.transAxes, ha='center', fontsize=9)
    from matplotlib.cm import ScalarMappable
    from matplotlib.colors import Normalize
    sm=ScalarMappable(norm=Normalize(low, high), cmap=cmap); sm.set_array([])
    cb=fig.colorbar(sm, ax=ax, shrink=.62, pad=.08); cb.set_label('Median throughput (M items/s)\nscale local to cube', fontsize=9)
    fig.text(.5, .025, f'{n} display bins retain CPU order; bin membership and complete/missing voxel coverage: topology-voxel-cube-coverage.json. {meta.get("placement_confidence", "")}', ha='center', fontsize=8)
    fig.savefig(path, dpi=180, facecolor='white')
    plt.close(fig)

def png_depth(rows, path: Path, meta: dict):
    plt, np, cmap = raster_modules()
    widths=sorted({int(r['width']) for r in rows}); series=[]
    for width in widths:
        values=[float(r['median_mps']) for r in rows if int(r['width']) == width]
        series.append((width, min(values), median(values), max(values)))
    fig, ax=plt.subplots(figsize=(11, 6), layout='constrained')
    x=np.arange(len(series)); lo=np.array([v[1] for v in series]); md=np.array([v[2] for v in series]); hi=np.array([v[3] for v in series])
    ax.vlines(x, lo, hi, color='#52657d', linewidth=2, label='min–max directed-pair median'); ax.scatter(x, md, c='#e53935', s=55, zorder=3, label='median across directed pairs')
    ax.set_xticks(x, ['scalar' if w == 0 else f'width {w}' for w, *_ in series]); ax.set_ylabel('Throughput (M items/s)'); ax.set_xlabel('API / fixed batch width'); ax.grid(axis='y', alpha=.25); ax.legend(); ax.set_title('Topology distribution by API / batch mode', weight='bold'); ax.text(.5, 1.01, meta.get('placement_confidence', ''), transform=ax.transAxes, ha='center', fontsize=8)
    fig.savefig(path, dpi=180, facecolor='white'); plt.close(fig)

def render(rows, out: Path, meta: dict, max_cpus: int):
    max_width=max((int(r['width']) for r in rows), default=0)
    png_heatmap(rows, 0, out/'scalar-heatmap.png', meta)
    png_heatmap(rows, max_width, out/f'fixed-{max_width}-heatmap.png', meta)
    png_depth(rows, out/'width-depth.png', meta)
    png_voxel_cube(rows, out/'topology-voxel-cube.png', meta, max_cpus)

def main():

    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--out',type=Path,default=ROOT/'docs'/'topology-matrix'); p.add_argument('--max-cpus',type=int,default=0); p.add_argument('--3d-max-cpus',type=int,default=10,help='ordered CPU display bins per cube dimension; 0 uses 10 bins'); p.add_argument('--transfers',type=int,default=2162160,help='calibration transfers; exact multiple of all fixed widths'); p.add_argument('--min-sample-ms',type=int,default=0,help='calibrate every producer/to/width cell to at least this timed duration; 0 disables'); p.add_argument('--rounds',type=int,default=3); p.add_argument('--widths',default='',help='comma-separated widths; 0=scalar, empty=all supported widths'); p.add_argument('--warmups',type=int,default=1); p.add_argument('--producer-shard',type=int,default=0,help='zero-based producer-row shard'); p.add_argument('--producer-shards',type=int,default=1,help='total non-overlapping producer-row shards'); p.add_argument('--no-build',action='store_true'); p.add_argument('--render-only',action='store_true',help='regenerate PNGs from existing results.csv and metadata.json'); a=p.parse_args()
    a.out.mkdir(parents=True,exist_ok=True)
    csv_path=a.out/'results.csv'
    if a.render_only:
        if not csv_path.exists(): raise SystemExit(f'--render-only needs {csv_path}')
        meta=json.loads((a.out/'metadata.json').read_text()) if (a.out/'metadata.json').exists() else metadata()
        rows=aggregate(load(csv_path)); (a.out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
        render(rows, a.out, meta, a.__dict__['3d_max_cpus'])
        print(f'Rendered PNG artifacts from {csv_path}')
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
    render(rows, a.out, meta, a.__dict__['3d_max_cpus'])
    print(f'Wrote {csv_path}, summary.json, scalar-heatmap.png, fixed-{max((int(r["width"]) for r in rows), default=0)}-heatmap.png, width-depth.png, topology-voxel-cube.png')
if __name__ == '__main__': main()
