/* ===== WYKRES 24h z tooltipem ===== */
let histData = {pv:[],ld:[],gr:[],step:300,range:'24h'};
let crosshairLine = null;

function lineChart(pv,ld,gr,step,range,mode){
 histData = {pv,ld,gr,step,range,mode};
 if(!mode) mode = 'all';
 const W=560,H=190,L=36,R=8,Tp=8,B=20,n=pv.length,s=$('hist');
 if(!s) return;
 if(n<2){s.innerHTML='<text x="20" y="50" fill="#8aa0b8" font-size="12">Zbieranie danych…</text>';return;}

 // Determine which series to show based on mode
 let showPv = mode === 'all' || mode === 'pv';
 let showLd = mode === 'all' || mode === 'load';
 let showGr = mode === 'all' || mode === 'grid';
 let series = [];
 if(showPv) series.push(pv);
 if(showLd) series.push(ld);
 if(showGr) series.push(gr);
 let mx = series.length ? Math.max(100, ...series.map(s => s.reduce((a, b) => Math.max(a, b), 0))) : 100;
 const u = mx > 2000 ? 1000 : 500;
 mx = Math.ceil(mx/u)*u;

 const X = i => L + i * (W-L-R) / (n-1);
 const Y = v => Tp + (H-Tp-B) * (1 - v/mx);
 let g = '';
 for(let k=0;k<=4;k++){
   const v = mx*k/4, y = Y(v);
   g += `<line x1="${L}" x2="${W-R}" y1="${y}" y2="${y}" stroke="#223449"/><text x="${L-4}" y="${y+3}" text-anchor="end" font-size="9" fill="#8aa0b8">${(v/1000).toFixed(1)}</text>`;
 }
 const pts = a => a.map((v,i) => X(i).toFixed(1) + ',' + Y(v).toFixed(1)).join(' ');
 const now = Date.now();
 for(let k=0;k<=4;k++){
   const i = Math.round(k*(n-1)/4), t = new Date(now - (n-1-i)*step*1000);
   g += `<text x="${X(i)}" y="${H-5}" text-anchor="middle" font-size="9" fill="#8aa0b8">${String(t.getHours()).padStart(2,'0')}:${String(t.getMinutes()).padStart(2,'0')}</text>`;
 }

 // Filled area only for PV in 'all' or 'pv' mode
 if(showPv) g += `<polygon points="${X(0)},${Y(0)} ${pts(pv)} ${X(n-1)},${Y(0)}" fill="#fbbf2430"/>`;
 if(showPv) g += `<polyline points="${pts(pv)}" fill="none" stroke="#fbbf24" stroke-width="2"/>`;
 if(showLd) g += `<polyline points="${pts(ld)}" fill="none" stroke="#38bdf8" stroke-width="1.5"/>`;
 if(showGr) g += `<polyline points="${pts(gr)}" fill="none" stroke="#ec4899" stroke-width="1.5"/>`;

 // crosshair - adaptive color
 const isLight = document.documentElement.getAttribute('data-theme') === 'light';
 const crossColor = isLight ? '#1e293b' : '#fff';
 g += `<line id="crosshair" x1="${L}" y1="${Tp}" x2="${L}" y2="${H-B}" stroke="${crossColor}" stroke-width="1" stroke-dasharray="4 4" opacity="0" style="pointer-events:none"/>`;
 // overlay
 g += `<rect id="hoverarea" x="${L}" y="${Tp}" width="${W-L-R}" height="${H-Tp-B}" fill="transparent"/>`;
 s.innerHTML = g;
 s.onclick = () => openFullscreen('hist');

 const area = document.getElementById('hoverarea');
 const tip = $('tooltip');
 const cross = document.getElementById('crosshair');
 area.onmousemove = function(e){
  const rect = s.getBoundingClientRect();
  const mx = (e.clientX - rect.left) * (560 / rect.width);
  let idx = Math.round((mx - L) / ((W-L-R)/(n-1)));
  idx = Math.max(0, Math.min(n-1, idx));
  const cx = X(idx);
  cross.setAttribute('x1', cx); cross.setAttribute('x2', cx);
  cross.style.opacity = '0.7';
  const t = new Date(Date.now() - (n-1-idx)*step*1000);
  let timeStr;
  if(range=='15m'||range=='1h'||range=='6h'||range=='24h') timeStr=`${String(t.getHours()).padStart(2,'0')}:${String(t.getMinutes()).padStart(2,'0')}`;
  else timeStr=`${String(t.getDate()).padStart(2,'0')}.${String(t.getMonth()+1).padStart(2,'0')} ${String(t.getHours()).padStart(2,'0')}:00`;

  let tooltipHtml = `<b>${timeStr}</b><br>`;
  if(showPv) tooltipHtml += `<span style="color:#fbbf24">PV: ${pv[idx]} W</span><br>`;
  if(showLd) tooltipHtml += `<span style="color:#38bdf8">Obciążenie: ${ld[idx]} W</span><br>`;
  if(showGr) tooltipHtml += `<span style="color:#ec4899">Sieć: ${gr[idx]} W</span><br>`;
  tip.innerHTML = tooltipHtml;
  tip.className = 'show';
  const box = $('histbox').getBoundingClientRect();
  let left = e.clientX - box.left + 12;
  let top = e.clientY - box.top - 10;
  if(left + 140 > box.width) left = e.clientX - box.left - 150;
  tip.style.left = left + 'px'; tip.style.top = top + 'px';
 };
 area.onmouseleave = ()=>{ tip.className = ''; cross.style.opacity = '0'; };
 area.ontouchstart = area.onmousemove;
 area.ontouchmove = area.onmousemove;
 area.ontouchend = ()=>{ tip.className = ''; cross.style.opacity = '0'; };
}