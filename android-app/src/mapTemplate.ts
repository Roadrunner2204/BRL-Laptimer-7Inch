/** Full Leaflet HTML embedded as a string for use in WebView */
export const MAP_HTML = `<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no"/>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<style>
  html,body{margin:0;padding:0;background:#0d0d0d;height:100%;overflow:hidden;}
  #map{width:100%;height:100vh;}
  .leaflet-tile-pane{filter:brightness(0.85) saturate(0.7);}
  .legend{position:fixed;bottom:12px;left:12px;background:rgba(0,0,0,0.75);
    border-radius:8px;padding:8px 12px;color:#fff;font-family:sans-serif;font-size:12px;
    pointer-events:none;z-index:1000;}
  .legend-row{display:flex;align-items:center;margin:3px 0;}
  .legend-dot{width:14px;height:4px;border-radius:2px;margin-right:8px;flex-shrink:0;}
  #status{position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);
    color:#888;font-family:sans-serif;font-size:16px;text-align:center;pointer-events:none;}
</style>
</head>
<body>
<div id="map"></div>
<div id="status">Lade Karte...</div>
<div class="legend" id="legend" style="display:none"></div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
const LAP_COLORS = ['#00C8FF','#BB88FF','#FF8800','#FF44AA','#66E0FF','#FFEE55','#FF5555','#44FFAA'];
const BEST_COLOR = '#0096FF';

const map = L.map('map',{zoomControl:true,attributionControl:false});
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:20,attribution:'© OpenStreetMap'}).addTo(map);
L.control.attribution({position:'bottomright',prefix:''}).addTo(map);

let polylines = [];  // [{lapIdx, lines:[L.Polyline], color, label}]
let markerSF = null;
let session = null;

function haversineM(lat1,lon1,lat2,lon2){
  const R=6371000,r=Math.PI/180;
  const dLat=(lat2-lat1)*r, dLon=(lon2-lon1)*r;
  const a=Math.sin(dLat/2)**2+Math.cos(lat1*r)*Math.cos(lat2*r)*Math.sin(dLon/2)**2;
  return 2*R*Math.asin(Math.sqrt(a));
}

function speedKmh(p1,p2){
  const dt=(p2[2]-p1[2])/1000;
  if(dt<=0||dt>10) return 0;
  return haversineM(p1[0],p1[1],p2[0],p2[1])/dt*3.6;
}

function speedToHex(kmh){
  // 0→blue, 60→green, 130→yellow, 200+→red
  const stops=[
    {v:0,  r:0,  g:120,b:255},
    {v:60, r:0,  g:204,b:102},
    {v:130,r:255,g:200,b:0},
    {v:200,r:255,g:40, b:0},
  ];
  let s0=stops[0],s1=stops[stops.length-1];
  for(let i=1;i<stops.length;i++){
    if(kmh<=stops[i].v){s0=stops[i-1];s1=stops[i];break;}
  }
  const t=Math.min(1,Math.max(0,(kmh-s0.v)/(s1.v-s0.v)));
  const lerp=(a,b)=>Math.round(a+(b-a)*t);
  const r=lerp(s0.r,s1.r),g=lerp(s0.g,s1.g),b=lerp(s0.b,s1.b);
  return '#'+[r,g,b].map(v=>v.toString(16).padStart(2,'0')).join('');
}

function drawLap(lap, lapIdx, bestIdx, showSpeed){
  const pts = lap.track_points;
  if(!pts||pts.length<2) return [];
  const isBest = lapIdx===bestIdx;
  const baseColor = isBest ? BEST_COLOR : LAP_COLORS[lapIdx % LAP_COLORS.length];
  const opacity = isBest ? 0.95 : 0.5;
  const weight = isBest ? 3.5 : 2;
  const segs = [];

  if(showSpeed && isBest){
    // Speed-colored segments for best lap
    for(let i=0;i<pts.length-1;i++){
      const kmh=speedKmh(pts[i],pts[i+1]);
      const color=speedToHex(kmh);
      const seg=L.polyline([[pts[i][0],pts[i][1]],[pts[i+1][0],pts[i+1][1]]],
        {color,weight:3.5,opacity:0.95,lineCap:'round'}).addTo(map);
      segs.push(seg);
    }
  } else {
    const coords=pts.map(p=>[p[0],p[1]]);
    const line=L.polyline(coords,{color:baseColor,weight,opacity,lineCap:'round'}).addTo(map);
    line.bindTooltip(\`Runde \${lap.lap}: \${fmtMs(lap.total_ms)}\`,{sticky:true});
    segs.push(line);
  }
  return segs;
}

function fmtMs(ms){
  const m=Math.floor(ms/60000);
  const s=((ms%60000)/1000).toFixed(3);
  return m+':'+s.padStart(6,'0');
}

function renderLegend(sess, visMap){
  const el=document.getElementById('legend');
  el.style.display='block';
  el.innerHTML='<b style="font-size:11px;color:#aaa">RUNDEN</b>';
  sess.laps.forEach((lap,i)=>{
    const color=i===sess.best_lap_idx ? BEST_COLOR : LAP_COLORS[i%LAP_COLORS.length];
    const vis=visMap[i]!==false;
    el.innerHTML+=\`<div class="legend-row">
      <div class="legend-dot" style="background:\${color};opacity:\${vis?1:0.3}"></div>
      <span style="opacity:\${vis?1:0.4}">R\${lap.lap} \${fmtMs(lap.total_ms)}\${i===sess.best_lap_idx?' ★':''}</span>
    </div>\`;
  });
}

let visMap = {};
let showSpeed = true;

function loadSession(data){
  // Clear
  polylines.forEach(g=>g.forEach(l=>map.removeLayer(l)));
  polylines=[];
  if(markerSF){map.removeLayer(markerSF);markerSF=null;}
  document.getElementById('status').style.display='none';

  session=data;
  visMap={};
  data.laps.forEach((_,i)=>visMap[i]=true);

  const allCoords=[];
  data.laps.forEach((lap,i)=>{
    const segs=drawLap(lap,i,data.best_lap_idx,showSpeed);
    polylines.push(segs);
    if(lap.track_points&&lap.track_points.length>0){
      lap.track_points.forEach(p=>allCoords.push([p[0],p[1]]));
    }
  });

  // Start/finish marker on best lap first point
  const best=data.laps[data.best_lap_idx];
  if(best&&best.track_points&&best.track_points.length>0){
    const pt=best.track_points[0];
    markerSF=L.circleMarker([pt[0],pt[1]],{radius:6,color:'#fff',fillColor:'#ff4444',fillOpacity:1,weight:2}).addTo(map);
    markerSF.bindTooltip('Start/Ziel',{permanent:false});
  }

  if(allCoords.length>0){
    map.fitBounds(L.latLngBounds(allCoords),{padding:[30,30],animate:false});
  }

  renderLegend(data,visMap);
}

function toggleLap(lapIdx, visible){
  if(!session) return;
  visMap[lapIdx]=visible;
  const segs=polylines[lapIdx];
  if(!segs) return;
  segs.forEach(l=>visible ? map.addLayer(l) : map.removeLayer(l));
  renderLegend(session,visMap);
}

function setSpeedColor(enabled){
  showSpeed=enabled;
  if(session) loadSession(session);
}

// Scrub-cursor car marker (moves along best lap at given distance)
let cursorMarker = null;
let cursorDistArr = null;  // cumulative distances for best lap
let cursorPoints  = null;  // track points of best lap

function ensureCursorData(){
  if(!session) return;
  const best = session.laps[session.best_lap_idx];
  if(!best || !best.track_points) return;
  cursorPoints = best.track_points;
  cursorDistArr = [0];
  for(let i=1;i<cursorPoints.length;i++){
    cursorDistArr.push(
      cursorDistArr[i-1] +
      haversineM(cursorPoints[i-1][0],cursorPoints[i-1][1],
                 cursorPoints[i][0],  cursorPoints[i][1])
    );
  }
}

function setCursorDist(d){
  if(!session) return;
  if(!cursorDistArr) ensureCursorData();
  if(!cursorDistArr || cursorDistArr.length===0) return;
  if(d==null){
    if(cursorMarker){map.removeLayer(cursorMarker);cursorMarker=null;}
    return;
  }
  // Find index where cumulative distance crosses d
  let j=0;
  while(j<cursorDistArr.length-1 && cursorDistArr[j+1]<d) j++;
  const p = cursorPoints[j];
  if(!p) return;
  if(!cursorMarker){
    cursorMarker = L.circleMarker([p[0],p[1]],
      {radius:9,color:'#fff',fillColor:BEST_COLOR,fillOpacity:1,weight:3,
       className:'cursor-marker'}).addTo(map);
  } else {
    cursorMarker.setLatLng([p[0],p[1]]);
  }
}

// Message bridge from React Native
function handleMsg(raw){
  try{
    const msg=JSON.parse(raw);
    if(msg.type==='session')   loadSession(msg.data);
    if(msg.type==='toggleLap') toggleLap(msg.lapIdx, msg.visible);
    if(msg.type==='speedColor') setSpeedColor(msg.enabled);
    if(msg.type==='cursor')    setCursorDist(msg.dist);
  }catch(e){console.error('msg error',e);}
}
document.addEventListener('message',e=>handleMsg(e.data));
window.addEventListener('message',e=>handleMsg(e.data));
</script>
</body>
</html>`;
