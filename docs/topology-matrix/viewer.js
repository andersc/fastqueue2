/* Static GitHub Pages viewer. Requires HTTPS hosting because it fetches JSON. */
const rainbow = d3.interpolateRgbBasis(['#0000ff','#00bfff','#00ff00','#ffff00','#ff7f00','#ff0000']);
const $ = id => document.getElementById(id);
let catalog, run, rows, meta, selected;
let stopScene = () => {};

async function json(path) { const r = await fetch(path); if (!r.ok) throw Error(`${path}: ${r.status}`); return r.json(); }
function rootFor(id) { return `linux-runs/${id}`; }
function domainMap() {
  const found = meta.topology_domains?.domains || [];
  return new Map(found.flatMap(d => (d.cpus || []).map(cpu => [+cpu, String(d.id)])));
}
function cpuOrder() { return (meta.selected_cpus || [...new Set(rows.flatMap(r => [+r.producer_cpu, +r.consumer_cpu]))]).map(Number).sort((a,b)=>a-b); }
function domainBoundaries(cpus, domains) { return d3.range(1, cpus.length).filter(i => domains.get(cpus[i]) !== domains.get(cpus[i - 1])); }
function labelWidth(w) { return +w === 0 ? 'scalar' : `fixed width ${w}`; }
function setLinks() { const base = rootFor(run.id); $('raw-link').href=`${base}/results.csv`; $('summary-link').href=`${base}/summary.json`; $('metadata-link').href=`${base}/metadata.json`; }

async function loadRun() {
  run = catalog.runs.find(x => x.id === $('run').value);
  [rows, meta] = await Promise.all([json(`${rootFor(run.id)}/summary.json`), json(`${rootFor(run.id)}/metadata.json`)]);
  const widths = [...new Set(rows.map(r => +r.width))].sort((a,b)=>a-b);
  $('width').replaceChildren(...widths.map(w => new Option(labelWidth(w), w)), new Option('all widths (3D layers)', 'all'));
  updateWidthControl();
  $('subtitle').textContent = `${run.label} · ${rows.length.toLocaleString()} exact measured path×mode medians`;
  setLinks(); selected = null; render();
}
function valuesForWidth(width) { return rows.filter(r => +r.width === +width); }
function allWidthsSelected() { return $('width').value === 'all'; }
function updateWidthControl() {
  const all = $('width').querySelector('option[value=all]');
  all.disabled = false;
}
function render() {
  updateWidthControl();
  const allWidths = allWidthsSelected(), width = allWidths ? null : +$('width').value, modeRows = allWidths ? rows : valuesForWidth(width), cpus = cpuOrder(), domains = domainMap();
  const data = new Map((allWidths ? [] : modeRows).map(r => [`${r.producer_cpu}/${r.consumer_cpu}`, r]));
  const scaleRows = $('scale').value === 'run' || allWidths ? rows : modeRows;
  const extent = d3.extent(scaleRows, r => +r.median_mps), color = d3.scaleSequential(rainbow).domain(extent);
  $('scale-label').textContent = `${extent[0].toFixed(2)}–${extent[1].toFixed(2)} M items/s (${ $('scale').value === 'run' || allWidths ? 'shared system scale' : 'selected-mode scale'})`;
  $('heatmap-title').textContent = allWidths ? 'All widths: 3D layers selected (choose one mode for 2D heatmap)' : `${labelWidth(width)}: producer → consumer median throughput`;
  drawHeatmap(cpus, domains, data, color); drawScene(cpus, domains, data, color, allWidths); drawComparison();
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
function drawScene(cpus, domains, data, color, allWidths) {
  const points = allWidths || $('scene-matrix-style').value === 'points';
  $('scene-matrix-style').disabled = allWidths;
  $('scene-title').childNodes[0].textContent = '3D throughput heatmap ';
  $('scene-note').textContent = allWidths
    ? 'Scientific 3D matrix layers: consumer CPU is X, producer CPU is Z, and vertical layers are scalar then fixed widths 1–8. Every flat point is one exact measured cell; missing cells/layers stay empty.'
    : `Scientific 3D matrix: consumer CPU is X, producer CPU is Z. Exact selected-mode median controls color${points ? '; points stay on plane' : ' and bar height'}. This is not a smoothed surface.`;
  $('scene-help').textContent = `Drag to orbit. Ctrl/⌘ + drag to pan. Scroll or pinch to zoom closer or farther out. Hover a ${allWidths ? 'layer point' : points ? 'point' : 'bar'} for its exact directed-path median and width; click to select it. Diagonal and missing measurements are absent. Display filter hides cells only—viewer never aggregates or invents values. OS “reduce motion” preference disables auto-rotation.`;
  drawMatrixScene(cpus, domains, data, color, points, allWidths);
}
function drawMatrixScene(cpus, domains, data, color, points, allWidths) {
 const host=$('scene'); stopScene(); hideTip(); host.replaceChildren();
 if (!window.THREE || !webglAvailable()) { sceneFallback(host); return; }
 const W=Math.max(320,host.clientWidth),H=470,renderer=createRenderer(W,H);
 if (!renderer) { sceneFallback(host); return; }
 const scene=new THREE.Scene(),camera=new THREE.PerspectiveCamera(42,W/H,.1,300);
 host.append(renderer.domElement);
 const filter=$('filter').value, extent=d3.extent($('scale').value==='run'?rows:[...data.values()],r=>+r.median_mps), lo=extent[0], hi=extent[1], span=Math.max(hi-lo,Number.EPSILON), side=Math.max(cpus.length,2), pitch=14/side;
 const cells=(allWidths ? rows : [...data.values()]).filter(r=>{const p=+r.producer_cpu,c=+r.consumer_cpu,same=domains.get(p)===domains.get(c); return p!==c && !((filter==='local'&&!same)||(filter==='remote'&&same));});
 const layers=[...new Set(cells.map(r=>+r.width))].sort((a,b)=>a-b), layerHeight=allWidths ? Math.max(4, layers.length*.72) : 0;
 const index=new Map(cpus.map((cpu,i)=>[cpu,i])), marks=points ? makeMatrixPoints(cells, index, side, pitch, color, allWidths ? layers : null) : makeMatrixBars(cells, index, side, pitch, lo, span, color); scene.add(marks);
 const floor=new THREE.GridHelper(Math.max(14,side*pitch),Math.min(side,64),0x365477,0x1a3551); floor.position.y=0; scene.add(floor);
 const frameHeight=allWidths ? layerHeight : 7.3, frame=new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.BoxGeometry(Math.max(14,side*pitch),frameHeight,Math.max(14,side*pitch))),new THREE.LineBasicMaterial({color:0x6fa6d6,transparent:true,opacity:.6})); frame.position.y=frameHeight/2; scene.add(frame);
 const labels=document.createElement('div'); labels.setAttribute('aria-hidden','true'); host.append(labels); let yaw=.65,pitchAngle=-.62,dist=22,drag,dragged=false,spin=false,hovered,animationFrame;
 const motionPreference=matchMedia('(prefers-reduced-motion: reduce)'); let reduced=motionPreference.matches;
 const applyMotionPreference=event=>{reduced=event.matches;if(reduced)spin=false;$('scene-spin').setAttribute('aria-pressed',spin);}; motionPreference.addEventListener?.('change',applyMotionPreference);
 const focusY=allWidths ? layerHeight/2 : 2, target=new THREE.Vector3(0,focusY,0);
 function pose(){camera.position.set(target.x+dist*Math.cos(pitchAngle)*Math.sin(yaw),target.y+dist*Math.sin(pitchAngle)+1,target.z+dist*Math.cos(pitchAngle)*Math.cos(yaw));camera.lookAt(target);camera.updateMatrixWorld();}
 function frameLoop(){if(spin&&!reduced)yaw+=.0025;pose();renderer.render(scene,camera);animationFrame=requestAnimationFrame(frameLoop);} frameLoop();
 function pointerToNdc(e){const rect=renderer.domElement.getBoundingClientRect();return new THREE.Vector2(((e.clientX-rect.left)/rect.width)*2-1,-((e.clientY-rect.top)/rect.height)*2+1);}
 renderer.domElement.addEventListener('pointerdown',e=>{hideTip();drag={x:e.clientX,y:e.clientY,pan:e.ctrlKey||e.metaKey};dragged=false;renderer.domElement.setPointerCapture(e.pointerId);}); renderer.domElement.addEventListener('pointermove',e=>{if(drag){hideTip();const dx=e.clientX-drag.x,dy=e.clientY-drag.y;dragged ||= Math.abs(dx)+Math.abs(dy)>2;if(drag.pan){const rect=renderer.domElement.getBoundingClientRect(), scale=dist/Math.min(rect.width,rect.height), forward=target.clone().sub(camera.position).normalize(), right=new THREE.Vector3().crossVectors(forward,camera.up).normalize(), up=new THREE.Vector3().crossVectors(right,forward).normalize();target.addScaledVector(right,-dx*scale).addScaledVector(up,dy*scale);}else{yaw+=dx*.008;pitchAngle=Math.max(-1.53,Math.min(1.53,pitchAngle+dy*.008));}drag.x=e.clientX;drag.y=e.clientY;return;}const ray=new THREE.Raycaster();ray.setFromCamera(pointerToNdc(e),camera);const hit=ray.intersectObject(marks,true)[0];if(hit){hovered=hit.object.userData.cells[Math.floor(hit.faceIndex/12)];showTip(e,hovered,+hovered.producer_cpu,+hovered.consumer_cpu,domains);}else{hovered=null;hideTip();}}); renderer.domElement.addEventListener('pointerleave',hideTip); renderer.domElement.addEventListener('pointerup',()=>drag=null); renderer.domElement.addEventListener('pointercancel',()=>{drag=null;hideTip();}); renderer.domElement.addEventListener('contextmenu',e=>{if(e.ctrlKey)e.preventDefault();}); renderer.domElement.addEventListener('click',()=>{if(!dragged&&hovered){selected={p:+hovered.producer_cpu,c:+hovered.consumer_cpu};drawComparison();}});renderer.domElement.addEventListener('wheel',e=>{e.preventDefault();dist=Math.max(4.5,Math.min(55,dist+e.deltaY*.012));},{passive:false});
 $('scene-home').onclick=()=>{yaw=.65;pitchAngle=-.62;dist=22;target.set(0,focusY,0);}; $('scene-spin').onclick=e=>{if(reduced)return;spin=!spin;e.currentTarget.setAttribute('aria-pressed',spin);};
 stopScene=()=>{motionPreference.removeEventListener?.('change',applyMotionPreference);cancelAnimationFrame(animationFrame);hideTip();marks.traverse(mark=>{mark.geometry?.dispose();mark.material?.dispose();});floor.geometry.dispose();floor.material.dispose();frame.geometry.dispose();frame.material.dispose();renderer.dispose();host.replaceChildren();stopScene=()=>{};};
}
function createRenderer(width,height) {
 try {
   const renderer=new THREE.WebGLRenderer({antialias:true,alpha:true});
   renderer.setPixelRatio(Math.min(devicePixelRatio,2)); renderer.setSize(width,height);
   if ('outputColorSpace' in renderer && THREE.SRGBColorSpace) renderer.outputColorSpace=THREE.SRGBColorSpace;
   else if ('outputEncoding' in renderer && THREE.sRGBEncoding) renderer.outputEncoding=THREE.sRGBEncoding;
   const probeScene=new THREE.Scene(), probeCamera=new THREE.PerspectiveCamera(45,1,.1,10), probeGeometry=new THREE.BoxGeometry(.5,.5,.5), probeMaterial=new THREE.MeshBasicMaterial({color:0xffffff});
   probeCamera.position.z=2; probeScene.add(new THREE.Mesh(probeGeometry,probeMaterial)); renderer.render(probeScene,probeCamera);
   const error=renderer.getContext().getError(); probeGeometry.dispose(); probeMaterial.dispose();
   if (error !== renderer.getContext().NO_ERROR) throw Error('WebGL render error '+error);
   return renderer;
 } catch (error) { console.warn('3D renderer unavailable',error); return null; }
}
function makeMatrixPoints(cells,index,side,pitch,color,layers=null) {
 const group=new THREE.Group(), chunkSize=7000, half=Math.max(pitch*.28,.018), height=Math.max(pitch*.04,.012), layerIndex=new Map((layers || []).map((w,i)=>[w,i])), layerStep=.72;
 const corners=[[-1,0,-1],[1,0,-1],[1,0,1],[-1,0,1],[-1,1,-1],[1,1,-1],[1,1,1],[-1,1,1]];
 const faces=[0,1,2,0,2,3,4,6,5,4,7,6,0,4,5,0,5,1,1,5,6,1,6,2,2,6,7,2,7,3,3,7,4,3,4,0];
 for(let offset=0;offset<cells.length;offset+=chunkSize) {
   const chunk=cells.slice(offset,offset+chunkSize), vertices=[], colors=[], indices=[];
   chunk.forEach((r,i)=>{const x=(index.get(+r.consumer_cpu)-(side-1)/2)*pitch, z=(index.get(+r.producer_cpu)-(side-1)/2)*pitch, rgb=new THREE.Color(color(+r.median_mps)); corners.forEach(([dx,dy,dz])=>{vertices.push(x+dx*half,(layerIndex.size ? layerIndex.get(+r.width)*layerStep : 0)+dy*height,z+dz*half);colors.push(rgb.r,rgb.g,rgb.b);}); faces.forEach(v=>indices.push(i*8+v));});
   const geometry=new THREE.BufferGeometry(); geometry.setAttribute('position',new THREE.Float32BufferAttribute(vertices,3)); geometry.setAttribute('color',new THREE.Float32BufferAttribute(colors,3)); geometry.setIndex(indices); geometry.computeBoundingSphere();
   const mesh=new THREE.Mesh(geometry,new THREE.MeshBasicMaterial({vertexColors:true,transparent:true,opacity:1,side:THREE.DoubleSide})); mesh.userData.cells=chunk; group.add(mesh);
 }
 return group;
}
function makeMatrixBars(cells,index,side,pitch,lo,span,color) {
 const group=new THREE.Group(), chunkSize=7000, half=pitch*.44;
 const corners=[[-1,0,-1],[1,0,-1],[1,0,1],[-1,0,1],[-1,1,-1],[1,1,-1],[1,1,1],[-1,1,1]];
 const faces=[0,1,2,0,2,3,4,6,5,4,7,6,0,4,5,0,5,1,1,5,6,1,6,2,2,6,7,2,7,3,3,7,4,3,4,0];
 for(let offset=0;offset<cells.length;offset+=chunkSize) {
   const chunk=cells.slice(offset,offset+chunkSize), vertices=[], colors=[], indices=[];
   chunk.forEach((r,i)=>{const norm=Math.max(.018,(+r.median_mps-lo)/span), h=.12+norm*7.1, x=(index.get(+r.consumer_cpu)-(side-1)/2)*pitch, z=(index.get(+r.producer_cpu)-(side-1)/2)*pitch, rgb=new THREE.Color(color(+r.median_mps)); corners.forEach(([dx,dy,dz])=>{vertices.push(x+dx*half,dy*h,z+dz*half);colors.push(rgb.r,rgb.g,rgb.b);}); faces.forEach(v=>indices.push(i*8+v));});
   const geometry=new THREE.BufferGeometry(); geometry.setAttribute('position',new THREE.Float32BufferAttribute(vertices,3)); geometry.setAttribute('color',new THREE.Float32BufferAttribute(colors,3)); geometry.setIndex(indices); geometry.computeBoundingSphere();
   const mesh=new THREE.Mesh(geometry,new THREE.MeshBasicMaterial({vertexColors:true,transparent:true,opacity:.94})); mesh.userData.cells=chunk; group.add(mesh);
 }
 return group;
}
function webglAvailable() { try { const canvas=document.createElement('canvas'); return !!(window.WebGLRenderingContext && (canvas.getContext('webgl2') || canvas.getContext('webgl'))); } catch (_) { return false; } }
function sceneFallback(host) { host.innerHTML='<p class="scene-fallback"><b>3D view unavailable in this browser.</b> WebGL could not start. Exact interactive 2D heatmap above remains available.</p>'; }
function showTip(event,r,p,c,domains) {
 const tip=d3.select('body').selectAll('#tip').data([null]).join('div').attr('class','tooltip').attr('id','tip');
 tip.html(`<b>CPU ${p} → CPU ${c}</b><br>${labelWidth(+r.width)}<br>${(+r.median_mps).toFixed(3)} M items/s<br>${domains.get(p)===domains.get(c)?'same domain':'cross-domain / interconnect'}<br>${r.sample_count} timed samples`); moveTip(event);
}
function moveTip(event) { d3.select('#tip').style('left',`${event.clientX+12}px`).style('top',`${event.clientY+12}px`); }
function hideTip() { d3.selectAll('#tip').remove(); }
function drawComparison() {
 const holder=d3.select('#comparison').html(''); if(!selected){$('details').innerHTML='<dt>Path</dt><dd>Hover and click heatmap cell</dd>'; return;}
 const rs=rows.filter(r=>+r.producer_cpu===selected.p && +r.consumer_cpu===selected.c).sort((a,b)=>+a.width-+b.width), max=d3.max(rs,r=>+r.median_mps), W=300,H=190,m={top:12,right:8,bottom:35,left:43};
 const x=d3.scaleBand().domain(rs.map(r=>+r.width)).range([m.left,W-m.right]).padding(.14), y=d3.scaleLinear().domain([0,max]).nice().range([H-m.bottom,m.top]); const svg=holder.append('svg').attr('width',W).attr('height',H);
 svg.append('g').selectAll('rect').data(rs).join('rect').attr('x',r=>x(+r.width)).attr('y',r=>y(+r.median_mps)).attr('width',x.bandwidth()).attr('height',r=>y(0)-y(+r.median_mps)).attr('fill',r=>rainbow((+r.median_mps)/max)); svg.append('g').attr('transform',`translate(0,${H-m.bottom})`).call(d3.axisBottom(x).tickFormat(labelWidth)); svg.append('g').attr('transform',`translate(${m.left},0)`).call(d3.axisLeft(y).ticks(4));
 $('details').innerHTML=`<dt>Path</dt><dd>CPU ${selected.p} → CPU ${selected.c}</dd><dt>Domain</dt><dd>${domainMap().get(selected.p)===domainMap().get(selected.c)?'same NUMA domain':'cross-NUMA / interconnect'}</dd><dt>Value</dt><dd>${allWidthsSelected() ? 'See selected 3D layer point' : (+rs.find(r=>+r.width===+$('width').value)?.median_mps).toFixed(3)+' M items/s'}</dd>`;
}
async function start() { try { catalog=await json('viewer-runs.json'); $('run').replaceChildren(...catalog.runs.map(r=>new Option(r.label,r.id))); for(const id of ['run','width','scale','filter','scene-matrix-style']) $(id).addEventListener('change', id==='run'?loadRun:render); await loadRun(); } catch(e) { $('subtitle').textContent=`Viewer failed: ${e.message}. Use HTTPS GitHub Pages, not file://.`; console.error(e); } }
start();
