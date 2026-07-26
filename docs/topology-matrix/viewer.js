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
  drawHeatmap(cpus, domains, data, color); drawScene(cpus, domains, data, color); drawComparison();
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
function drawScene(cpus, domains, data, color) {
 const host=$('scene'); host.replaceChildren(); if (!window.THREE) { host.innerHTML='<p class="scene-fallback">3D renderer unavailable. CDN request failed; exact accessible 2D heatmap remains available above.</p>'; return; }
 const W=Math.max(320,host.clientWidth), H=470, scene=new THREE.Scene(), camera=new THREE.PerspectiveCamera(45,W/H,.1,200), renderer=new THREE.WebGLRenderer({antialias:true,alpha:true});
 renderer.setPixelRatio(Math.min(devicePixelRatio,2)); renderer.setSize(W,H); host.append(renderer.domElement); camera.position.set(0,0,22);
 const group=new THREE.Group(); scene.add(group); const domIds=[...new Set(cpus.map(c=>domains.get(c)||'unknown'))], byDomain=new Map(domIds.map(id=>[id,cpus.filter(c=>(domains.get(c)||'unknown')===id)])), positions=new Map();
 [...byDomain.entries()].forEach(([id, list], di)=>{const orbit=di===0?5.3:8.2+di*1.5, offset=(di/domIds.length)*Math.PI*2; list.forEach((cpu,i)=>{const a=offset+i*Math.PI*2/list.length, z=(di-(domIds.length-1)/2)*1.9; positions.set(cpu,new THREE.Vector3(Math.cos(a)*orbit,Math.sin(a)*orbit,z));});});
 const nodeGeo=new THREE.SphereGeometry(.18,12,9); cpus.forEach(cpu=>{const mat=new THREE.MeshBasicMaterial({color:new THREE.Color('#9eeaff')}); const node=new THREE.Mesh(nodeGeo,mat); node.position.copy(positions.get(cpu)); group.add(node);});
 const filter=$('scene-paths').value, candidates=[...data.values()].filter(r=>{const p=+r.producer_cpu,c=+r.consumer_cpu,same=domains.get(p)===domains.get(c); return filter==='selected'?selected&&selected.p===p&&selected.c===c:filter==='local'?same:filter==='remote'?!same:true;}).sort((a,b)=>+b.median_mps-+a.median_mps); const display=filter==='top'?candidates.slice(0,500):candidates.slice(0,1200), links=[];
 display.forEach(r=>{const p=+r.producer_cpu,c=+r.consumer_cpu,a=positions.get(p),b=positions.get(c); if(!a||!b)return; const mid=a.clone().add(b).multiplyScalar(.5); mid.z+=.65+Math.min(3,(+r.median_mps)/120); const curve=new THREE.QuadraticBezierCurve3(a,mid,b), pts=curve.getPoints(16), geo=new THREE.BufferGeometry().setFromPoints(pts), mat=new THREE.LineBasicMaterial({color:color(+r.median_mps),transparent:true,opacity:.58}); const line=new THREE.Line(geo,mat); line.userData={r,p,c,pts}; group.add(line); links.push(line); });
 const labels=document.createElement('div'); labels.setAttribute('aria-hidden','true'); host.append(labels); let yaw=.22,pitch=-.45,dist=22,drag,spin=false,reduced=matchMedia('(prefers-reduced-motion: reduce)').matches, hovered;
 function project(v){const q=v.clone().applyMatrix4(group.matrixWorld).project(camera); return {x:(q.x*.5+.5)*W,y:(-q.y*.5+.5)*H,visible:q.z<1};} function labelNodes(){labels.replaceChildren(); if(cpus.length>64)return; cpus.forEach(cpu=>{const q=project(positions.get(cpu)); if(!q.visible)return; const el=document.createElement('span');el.className='scene-label';el.style.left=`${q.x}px`;el.style.top=`${q.y}px`;el.textContent=`CPU ${cpu}`;labels.append(el);});}
 function pose(){camera.position.set(dist*Math.cos(pitch)*Math.sin(yaw),dist*Math.sin(pitch),dist*Math.cos(pitch)*Math.cos(yaw));camera.lookAt(0,0,0);camera.updateMatrixWorld();group.updateMatrixWorld();} function frame(){if(spin&&!reduced)yaw+=.0025;pose();renderer.render(scene,camera);labelNodes();requestAnimationFrame(frame);} frame();
 renderer.domElement.addEventListener('pointerdown',e=>{drag=[e.clientX,e.clientY];renderer.domElement.setPointerCapture(e.pointerId);}); renderer.domElement.addEventListener('pointermove',e=>{if(drag){yaw+=(e.clientX-drag[0])*.008;pitch=Math.max(-1.3,Math.min(1.3,pitch+(e.clientY-drag[1])*.008));drag=[e.clientX,e.clientY];return;} const mouse=new THREE.Vector2(e.offsetX/W*2-1,-e.offsetY/H*2+1), ray=new THREE.Raycaster();ray.setFromCamera(mouse,camera); const hit=ray.intersectObjects(links)[0]; if(hit){hovered=hit.object.userData;showTip(e,hovered.r,hovered.p,hovered.c,domains);}else {hovered=null;hideTip();}}); renderer.domElement.addEventListener('pointerup',()=>drag=null); renderer.domElement.addEventListener('click',()=>{if(hovered){selected={p:hovered.p,c:hovered.c};drawComparison();}}); renderer.domElement.addEventListener('wheel',e=>{e.preventDefault();dist=Math.max(9,Math.min(45,dist+e.deltaY*.012));},{passive:false});
 $('scene-home').onclick=()=>{yaw=.22;pitch=-.45;dist=22;}; $('scene-spin').onclick=e=>{spin=!spin;e.currentTarget.setAttribute('aria-pressed',spin);}; $('scene-motion').onclick=e=>{reduced=!reduced;if(reduced)spin=false;e.currentTarget.setAttribute('aria-pressed',reduced);$('scene-spin').setAttribute('aria-pressed',spin);};
}
function showTip(event,r,p,c,domains) {
 d3.select('body').append('div').attr('class','tooltip').attr('id','tip').html(`<b>CPU ${p} → CPU ${c}</b><br>${(+r.median_mps).toFixed(3)} M items/s<br>${domains.get(p)===domains.get(c)?'same domain':'cross-domain / interconnect'}<br>${r.sample_count} timed samples`).style('left',`${event.clientX+12}px`).style('top',`${event.clientY+12}px`);
}
function moveTip(event) { d3.select('#tip').style('left',`${event.clientX+12}px`).style('top',`${event.clientY+12}px`); }
function hideTip() { d3.select('#tip').remove(); }
function drawComparison() {
 const holder=d3.select('#comparison').html(''); if(!selected){$('details').innerHTML='<dt>Path</dt><dd>Hover and click heatmap cell</dd>'; return;}
 const rs=rows.filter(r=>+r.producer_cpu===selected.p && +r.consumer_cpu===selected.c).sort((a,b)=>+a.width-+b.width), max=d3.max(rs,r=>+r.median_mps), W=300,H=190,m={top:12,right:8,bottom:35,left:43};
 const x=d3.scaleBand().domain(rs.map(r=>+r.width)).range([m.left,W-m.right]).padding(.14), y=d3.scaleLinear().domain([0,max]).nice().range([H-m.bottom,m.top]); const svg=holder.append('svg').attr('width',W).attr('height',H);
 svg.append('g').selectAll('rect').data(rs).join('rect').attr('x',r=>x(+r.width)).attr('y',r=>y(+r.median_mps)).attr('width',x.bandwidth()).attr('height',r=>y(0)-y(+r.median_mps)).attr('fill',r=>rainbow((+r.median_mps)/max)); svg.append('g').attr('transform',`translate(0,${H-m.bottom})`).call(d3.axisBottom(x).tickFormat(labelWidth)); svg.append('g').attr('transform',`translate(${m.left},0)`).call(d3.axisLeft(y).ticks(4));
 $('details').innerHTML=`<dt>Path</dt><dd>CPU ${selected.p} → CPU ${selected.c}</dd><dt>Domain</dt><dd>${domainMap().get(selected.p)===domainMap().get(selected.c)?'same NUMA domain':'cross-NUMA / interconnect'}</dd><dt>Value</dt><dd>${(+rs.find(r=>+r.width===+$('width').value)?.median_mps).toFixed(3)} M items/s</dd>`;
}
async function start() { try { catalog=await json('viewer-runs.json'); $('run').replaceChildren(...catalog.runs.map(r=>new Option(r.label,r.id))); for(const id of ['run','width','scale','filter','scene-paths']) $(id).addEventListener('change', id==='run'?loadRun:render); await loadRun(); } catch(e) { $('subtitle').textContent=`Viewer failed: ${e.message}. Use HTTPS GitHub Pages, not file://.`; console.error(e); } }
start();
