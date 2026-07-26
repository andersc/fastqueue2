/* Static GitHub Pages viewer. Requires HTTPS hosting because it fetches JSON. */
const rainbow = d3.interpolateRgbBasis(['#0000ff','#00bfff','#00ff00','#ffff00','#ff7f00','#ff0000']);
const $ = id => document.getElementById(id);
let catalog, run, rows, meta, selected;

async function json(path) { const r = await fetch(path); if (!r.ok) throw Error(`${path}: ${r.status}`); return r.json(); }
function rootFor(id) { return `linux-runs/${id}`; }
function domainMap() {
  const found = meta.topology_domains?.domains || [];
  return new Map(found.flatMap(d => (d.cpus || []).map(cpu => [+cpu, String(d.id)])));
}
function cpuOrder() { return (meta.allowed_cpus || [...new Set(rows.flatMap(r => [+r.producer_cpu, +r.consumer_cpu]))]).map(Number).sort((a,b)=>a-b); }
function domainBoundaries(cpus, domains) { return d3.range(1, cpus.length).filter(i => domains.get(cpus[i]) !== domains.get(cpus[i - 1])); }
function labelWidth(w) { return +w === 0 ? 'scalar' : `fixed width ${w}`; }
function setLinks() { const base = rootFor(run.id); $('raw-link').href=`${base}/results.csv`; $('summary-link').href=`${base}/summary.json`; $('metadata-link').href=`${base}/metadata.json`; }

async function loadRun() {
  run = catalog.runs.find(x => x.id === $('run').value);
  [rows, meta] = await Promise.all([json(`${rootFor(run.id)}/summary.json`), json(`${rootFor(run.id)}/metadata.json`)]);
  const widths = [...new Set(rows.map(r => +r.width))].sort((a,b)=>a-b);
  $('width').replaceChildren(...widths.map(w => new Option(labelWidth(w), w)));
  $('subtitle').textContent = `${run.label} · ${rows.length.toLocaleString()} exact directed path×mode medians · ${meta.placement_confidence || 'placement details in metadata'}`;
  setLinks(); selected = null; render();
}
function valuesForWidth(width) { return rows.filter(r => +r.width === +width); }
function render() {
  const width = +$('width').value, modeRows = valuesForWidth(width), cpus = cpuOrder(), domains = domainMap();
  const data = new Map(modeRows.map(r => [`${r.producer_cpu}/${r.consumer_cpu}`, r]));
  const all = $('scale').value === 'run' ? rows : modeRows;
  const extent = d3.extent(all, r => +r.median_mps), color = d3.scaleSequential(rainbow).domain(extent);
  $('scale-label').textContent = `${extent[0].toFixed(2)}–${extent[1].toFixed(2)} M items/s (${ $('scale').value === 'run' ? 'run' : 'mode'} scale)`;
  $('heatmap-title').textContent = `${labelWidth(width)}: producer → consumer median throughput`;
  drawHeatmap(cpus, domains, data, color); drawComparison();
}
function drawHeatmap(cpus, domains, data, color) {
  const holder = d3.select('#heatmap').html(''), max=760, cell=Math.max(10, Math.floor(max / cpus.length)), margin={top:38,right:12,bottom:48,left:48}, side=cell*cpus.length;
  const svg=holder.append('svg').attr('width',side+margin.left+margin.right).attr('height',side+margin.top+margin.bottom);
  const g=svg.append('g').attr('transform',`translate(${margin.left},${margin.top})`), idx=new Map(cpus.map((v,i)=>[v,i]));
  const filter=$('filter').value;
  for (const p of cpus) for (const c of cpus) { const r=data.get(`${p}/${c}`), same=domains.get(p) === domains.get(c); if (!r || p===c || (filter==='local'&&!same) || (filter==='remote'&&same)) continue;
    g.append('rect').attr('class','cell').attr('x',idx.get(c)*cell).attr('y',idx.get(p)*cell).attr('width',cell).attr('height',cell).attr('fill',color(+r.median_mps)).on('mouseenter',(event)=>showTip(event,r,p,c,domains)).on('mousemove',moveTip).on('mouseleave',hideTip).on('click',()=>{selected={p,c}; drawComparison();}); }
  for (const b of domainBoundaries(cpus,domains)) { g.append('line').attr('class','boundary').attr('x1',b*cell).attr('x2',b*cell).attr('y2',side); g.append('line').attr('class','boundary').attr('y1',b*cell).attr('y2',b*cell).attr('x2',side); }
  const axis=(selection, vertical)=>selection.selectAll('text').data(cpus).join('text').attr('font-size',Math.min(11,cell*.75)).attr(vertical?'y':'x',(d,i)=>i*cell+cell/2).attr(vertical?'x':'y',vertical?-6:side+13).attr('text-anchor',vertical?'end':'middle').text(d=>d);
  axis(g.append('g'),false); axis(g.append('g'),true);
}
function showTip(event,r,p,c,domains) { d3.select('body').append('div').attr('class','tooltip').attr('id','tip').html(`<b>CPU ${p} → CPU ${c}</b><br>${(+r.median_mps).toFixed(3)} M items/s<br>${domains.get(p)===domains.get(c)?'same domain':'cross-domain / interconnect'}<br>${r.sample_count} timed samples`).style('left',`${event.clientX+12}px`).style('top',`${event.clientY+12}px`); }
function moveTip(event) { d3.select('#tip').style('left',`${event.clientX+12}px`).style('top',`${event.clientY+12}px`); }
function hideTip() { d3.select('#tip').remove(); }
function drawComparison() {
 const holder=d3.select('#comparison').html(''); if(!selected){$('details').innerHTML='<dt>Path</dt><dd>Hover and click heatmap cell</dd>'; return;}
 const rs=rows.filter(r=>+r.producer_cpu===selected.p && +r.consumer_cpu===selected.c).sort((a,b)=>+a.width-+b.width), max=d3.max(rs,r=>+r.median_mps), W=300,H=190,m={top:12,right:8,bottom:35,left:43};
 const x=d3.scaleBand().domain(rs.map(r=>+r.width)).range([m.left,W-m.right]).padding(.14), y=d3.scaleLinear().domain([0,max]).nice().range([H-m.bottom,m.top]); const svg=holder.append('svg').attr('width',W).attr('height',H);
 svg.append('g').selectAll('rect').data(rs).join('rect').attr('x',r=>x(+r.width)).attr('y',r=>y(+r.median_mps)).attr('width',x.bandwidth()).attr('height',r=>y(0)-y(+r.median_mps)).attr('fill',r=>rainbow((+r.median_mps)/max)); svg.append('g').attr('transform',`translate(0,${H-m.bottom})`).call(d3.axisBottom(x).tickFormat(labelWidth)); svg.append('g').attr('transform',`translate(${m.left},0)`).call(d3.axisLeft(y).ticks(4));
 $('details').innerHTML=`<dt>Path</dt><dd>CPU ${selected.p} → CPU ${selected.c}</dd><dt>Domain</dt><dd>${domainMap().get(selected.p)===domainMap().get(selected.c)?'same NUMA domain':'cross-NUMA / interconnect'}</dd><dt>Value</dt><dd>${(+rs.find(r=>+r.width===+$('width').value)?.median_mps).toFixed(3)} M items/s</dd>`;
}
async function start() { try { catalog=await json('viewer-runs.json'); $('run').replaceChildren(...catalog.runs.map(r=>new Option(r.label,r.id))); for(const id of ['run','width','scale','filter']) $(id).addEventListener('change', id==='run'?loadRun:render); await loadRun(); } catch(e) { $('subtitle').textContent=`Viewer failed: ${e.message}. Use HTTPS GitHub Pages, not file://.`; console.error(e); } }
start();
