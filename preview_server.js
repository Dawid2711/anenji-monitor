const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 8080;

// Mock data for DEMO mode
const mockData = {
  ok: true,
  mode: 2,
  pv: 2450,
  pv1v: 345.2,
  soc: 68,
  load: 1890,
  grid: 0,
  bp: 560,
  bv: 27.8,
  bc: 20.1,
  ac: 231.5,
  freq: 49.98,
  out: 230.2,
  temp: 38,
  pvd: 12.4,
  ld: 18.7,
  gd: 0.3,
  rssi: -52,
  ip: '192.168.1.142',
  ap: false,
  ver: '2026.11.18',
  time: '14:32'
};

// Generate mock history data
function generateMockHistory(range) {
  let step = 300, n = 288;
  if (range === '15m') { n = 30; step = 300; }
  else if (range === '1h') { n = 12; step = 300; }
  else if (range === '6h') { n = 72; step = 300; }
  else if (range === '24h') { n = 288; step = 300; }
  else if (range === '7d') { n = 168; step = 3600; }
  else if (range === '30d') { n = 720; step = 3600; }
  else if (range === '90d') { n = 720; step = 10800; }
  else if (range === '1y') { n = 365; step = 86400; }

  const pv = [], ld = [], gr = [];
  const now = Date.now();
  for (let i = 0; i < n; i++) {
    const t = new Date(now - (n - 1 - i) * step * 1000);
    const hour = t.getHours();
    const dayFactor = hour > 6 && hour < 19 ? Math.sin(Math.PI * (hour - 6) / 13) : 0;
    const p = Math.round(Math.max(0, 3000 * dayFactor + 200 * Math.sin(i * 0.3)));
    const l = Math.round(600 + 400 * Math.sin(i / 20) + 150 * Math.sin(i * 1.5));
    pv.push(p);
    ld.push(l);
    gr.push(Math.max(0, l - p));
  }
  return { step, range, n, pv, ld, gr, days: [8.2, 6.5, 9.1, 7.8, 5.4, 10.2, 12.4] };
}

// Generate mock days data
function generateMockDays() {
  const rec = [];
  const now = Date.now();
  for (let d = 30; d >= 0; d--) {
    const date = new Date(now - d * 86400000);
    const dateStr = date.getFullYear() * 10000 + (date.getMonth() + 1) * 100 + date.getDate();
    const pv = Math.round((3 + Math.random() * 8) * 100) / 100;
    const ld = Math.round((5 + Math.random() * 10) * 100) / 100;
    const gd = Math.round(Math.max(0, ld - pv) * 100) / 100;
    rec.push([dateStr, pv, ld, gd]);
  }
  return { today: { d: new Date().getFullYear() * 10000 + (new Date().getMonth() + 1) * 100 + new Date().getDate(), pv: 12.4, ld: 18.7, gd: 0.3 }, rec };
}

// Mock config
const mockConfig = {
  id: 1,
  baud: 9600,
  bs: 1,
  off: 0,
  bco: 0,
  ghu: 'Dawid2711',
  ghr: 'anenji-monitor',
  ghe: true,
  sigs: [
    {k:'mode',n:'Tryb pracy',d:201,a:201,s:1,t:0,g:0,v:2,st:1},
    {k:'pv',n:'Moc PV',d:223,a:223,s:1,t:0,g:0,v:2450,st:1},
    {k:'load',n:'Moc obciążenia',d:213,a:213,s:1,t:0,g:0,v:1890,st:1},
    {k:'soc',n:'SOC baterii',d:229,a:229,s:1,t:0,g:0,v:68,st:1},
    {k:'bv',n:'Napięcie baterii',d:215,a:215,s:0.1,t:0,g:0,v:27.8,st:1},
    {k:'bc',n:'Prąd baterii (232)',d:232,a:232,s:0.1,t:0,g:1,v:20.1,st:1},
    {k:'ac',n:'Napięcie sieci AC',d:202,a:202,s:0.1,t:0,g:0,v:231.5,st:1},
    {k:'freq',n:'Częstotliwość sieci',d:203,a:203,s:0.01,t:0,g:0,v:49.98,st:1},
    {k:'out',n:'Napięcie wyjściowe',d:210,a:210,s:0.1,t:0,g:0,v:230.2,st:1},
    {k:'temp',n:'Temperatura falownika',d:227,a:227,s:1,t:0,g:1,v:38,st:1},
    {k:'pv1v',n:'Napięcie PV',d:219,a:219,s:0.1,t:0,g:0,v:345.2,st:1},
    {k:'grid',n:'Moc z sieci (204)',d:204,a:204,s:1,t:0,g:1,v:0,st:1},
  ]
};

// Mock GitHub status
const mockGhStatus = {
  enable: true,
  user: 'Dawid2711',
  repo: 'anenji-monitor',
  latest: '2026.11.18',
  newver: false,
  ver: '2026.11.18',
  err: 0,
  msg: 'Najnowsza: 2026.11.18 · twoja 2026.11.18 · zgodne',
  updating: false,
  bytes: 0
};

const mimeTypes = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'application/javascript',
  '.css': 'text/css',
  '.json': 'application/json',
  '.bin': 'application/octet-stream',
  '.txt': 'text/plain'
};

const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://localhost:${PORT}`);
  const pathname = url.pathname;

  // Enable CORS
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  // API endpoints
  if (pathname === '/data') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(mockData));
    return;
  }

  if (pathname === '/history') {
    const range = url.searchParams.get('range') || '24h';
    const data = generateMockHistory(range);
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(data));
    return;
  }

  if (pathname === '/days') {
    const data = generateMockDays();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(data));
    return;
  }

  if (pathname === '/cfg') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(mockConfig));
    return;
  }

  if (pathname === '/ghstatus') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(mockGhStatus));
    return;
  }

  if (pathname === '/ghcheck' || pathname === '/ghupdate') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true }));
    return;
  }

  if (pathname === '/days.csv') {
    const data = generateMockDays();
    let csv = '﻿data;pv_kwh;zuzycie_kwh;siec_kwh\r\n';
    data.rec.forEach(r => {
      csv += `${String(r[0]).slice(0,4)}-${String(r[0]).slice(4,6)}-${String(r[0]).slice(6,8)};${r[1].toFixed(2).replace('.',',')};${r[2].toFixed(2).replace('.',',')};${r[3].toFixed(2).replace('.',',')}\r\n`;
    });
    res.writeHead(200, { 'Content-Type': 'text/csv; charset=utf-8', 'Content-Disposition': 'attachment; filename=anenji-historia.csv' });
    res.end(csv);
    return;
  }

  // Static files
  let filePath = pathname === '/' ? '/preview.html' : pathname;
  filePath = path.join(__dirname, filePath);

  fs.readFile(filePath, (err, content) => {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end('Not found');
      return;
    }
    const ext = path.extname(filePath);
    res.writeHead(200, { 'Content-Type': mimeTypes[ext] || 'text/plain' });
    res.end(content);
  });
});

server.listen(PORT, () => {
  console.log(`Preview server running at http://localhost:${PORT}`);
  console.log('Open in browser to see the UI with mock data');
});