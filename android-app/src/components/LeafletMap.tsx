/**
 * LeafletMap — embeddable OpenStreetMap for racing-line visualization.
 *
 * Lightweight wrapper around a Leaflet WebView. The parent passes lap
 * traces (with color) and an optional cursor distance; the map auto-fits,
 * draws polylines, and puts a marker along the primary lap.
 */

import React, { useEffect, useMemo, useRef } from 'react';
import { View, StyleSheet } from 'react-native';
import { WebView } from 'react-native-webview';
import { C } from '../theme';

export interface LeafletLap {
  lapIdx: number;
  color: string;
  /** [lat, lon] per sample, aligned with cumDist */
  points: [number, number][];
  /** Cumulative distance in meters per point, same length as points */
  cumDist: number[];
  label?: string;
}

interface Props {
  laps: LeafletLap[];
  /** Which lap's distance is used for cursor lookup */
  primaryLapIdx?: number;
  /** Distance in meters along primary lap */
  cursorDist?: number;
  height?: number;
}

export default function LeafletMap({ laps, primaryLapIdx, cursorDist, height = 240 }: Props) {
  const webviewRef = useRef<WebView>(null);
  const readyRef = useRef(false);

  const html = useMemo(() => HTML, []);

  function postMsg(msg: any) {
    if (!readyRef.current) return;
    webviewRef.current?.injectJavaScript(
      `handleMsg(${JSON.stringify(JSON.stringify(msg))}); true;`
    );
  }

  // Initial / lap-change payload
  useEffect(() => {
    postMsg({ type: 'laps', laps, primaryLapIdx });
  }, [laps, primaryLapIdx]);

  useEffect(() => {
    postMsg({ type: 'cursor', dist: cursorDist });
  }, [cursorDist]);

  return (
    <View style={[styles.wrap, { height }]}>
      <WebView
        ref={webviewRef}
        source={{ html }}
        style={styles.map}
        javaScriptEnabled
        domStorageEnabled
        originWhitelist={['*']}
        mixedContentMode="always"
        onLoadEnd={() => {
          readyRef.current = true;
          postMsg({ type: 'laps', laps, primaryLapIdx });
          if (cursorDist != null) postMsg({ type: 'cursor', dist: cursorDist });
        }}
        androidLayerType="hardware"
      />
    </View>
  );
}

const styles = StyleSheet.create({
  wrap: { borderRadius: 10, overflow: 'hidden', backgroundColor: C.surface },
  map:  { flex: 1, backgroundColor: C.surface },
});

// --- Leaflet HTML --------------------------------------------------------
const HTML = `<!DOCTYPE html>
<html><head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no"/>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<style>
  html,body{margin:0;padding:0;background:#0d1117;height:100%;overflow:hidden;}
  #map{width:100%;height:100vh;background:#0d1117;}
  .leaflet-tile-pane{filter:brightness(0.82) saturate(0.8);}
  .leaflet-container{background:#0d1117;}
  .leaflet-control-zoom a{background:rgba(13,17,23,0.85);color:#fff;border:none;}
  .leaflet-control-zoom a:hover{background:#0096FF;color:#000;}
</style>
</head><body>
<div id="map"></div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
const map = L.map('map',{zoomControl:true,attributionControl:false});
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:20}).addTo(map);
map.setView([48.2,13.8], 13);  // placeholder until data arrives

let lines = [];
let cursorMarker = null;
let primaryCumDist = null;
let primaryPoints  = null;
let fitZoom = null;   // zoom level after fitBounds (=> "overview" zoom)

function drawLaps(msg){
  lines.forEach(l => map.removeLayer(l));
  lines = [];
  const allCoords = [];
  let primary = null;
  msg.laps.forEach(lap => {
    const coords = lap.points.map(p => [p[0], p[1]]);
    if(coords.length < 2) return;
    const isPrimary = lap.lapIdx === msg.primaryLapIdx;
    const line = L.polyline(coords, {
      color: lap.color,
      weight: isPrimary ? 3.5 : 2,
      opacity: isPrimary ? 0.95 : 0.55,
      lineCap: 'round',
      lineJoin: 'round',
    }).addTo(map);
    lines.push(line);
    coords.forEach(c => allCoords.push(c));
    if(isPrimary) primary = lap;
  });
  if(!primary && msg.laps.length > 0) primary = msg.laps[0];
  if(primary){
    primaryPoints  = primary.points;
    primaryCumDist = primary.cumDist;
  }
  if(allCoords.length > 0){
    map.fitBounds(L.latLngBounds(allCoords),{padding:[24,24],animate:false});
    fitZoom = map.getZoom();
  }
}

function setCursor(dist){
  if(dist == null){
    if(cursorMarker){ map.removeLayer(cursorMarker); cursorMarker = null; }
    return;
  }
  if(!primaryCumDist || !primaryPoints || primaryCumDist.length < 2) return;
  let j = 0;
  while(j < primaryCumDist.length - 1 && primaryCumDist[j+1] < dist) j++;
  const p = primaryPoints[j];
  if(!p) return;
  const latlng = [p[0], p[1]];
  if(!cursorMarker){
    cursorMarker = L.circleMarker(latlng, {
      radius: 9, color: '#fff', fillColor: '#0096FF',
      fillOpacity: 1, weight: 3,
    }).addTo(map);
  } else {
    cursorMarker.setLatLng(latlng);
  }
  // Auto-follow: once user has zoomed in past the full-track fit level,
  // pan the map so the cursor stays visible. At overview zoom we keep the
  // whole track framed and don't steal the view.
  if(fitZoom != null && map.getZoom() > fitZoom){
    const b = map.getBounds().pad(-0.2);  // inner 60% of the view
    if(!b.contains(latlng)){
      map.panTo(latlng, { animate: true, duration: 0.2, easeLinearity: 0.5 });
    }
  }
}

function handleMsg(raw){
  try{
    const msg = JSON.parse(raw);
    if(msg.type === 'laps')   drawLaps(msg);
    if(msg.type === 'cursor') setCursor(msg.dist);
  }catch(e){}
}
document.addEventListener('message', e => handleMsg(e.data));
window.addEventListener('message',  e => handleMsg(e.data));
</script>
</body></html>`;
