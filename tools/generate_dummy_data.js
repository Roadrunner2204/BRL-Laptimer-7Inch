/**
 * generate_dummy_data.js
 * Generates Salzburgring dummy sessions for BRL Laptimer SD card.
 * Run: node generate_dummy_data.js
 * Then copy the output folders to the SD card root.
 */

const fs = require('fs');
const path = require('path');

// ── Salzburgring GPS waypoints (approximate real layout) ─────────────────────
// Circuit runs ~4.24 km, counter-clockwise
// Each waypoint: [lat, lon, cumulative_seconds]
const WAYPOINTS = [
  // Start/Finish main straight heading east→right hairpin
  [47.78330, 13.18460,  0.0],   // S/F line
  [47.78328, 13.18400,  1.5],
  [47.78325, 13.18330,  3.2],
  [47.78318, 13.18240,  5.1],
  [47.78310, 13.18170,  6.8],   // braking for hairpin 1
  // Hairpin right (Reifnitz-Kurve)
  [47.78305, 13.18120,  8.0],
  [47.78310, 13.18075, 10.0],
  [47.78330, 13.18055, 12.0],   // apex
  [47.78355, 13.18068, 13.8],
  [47.78378, 13.18100, 15.2],
  // Exit hairpin → back straight heading east
  [47.78395, 13.18140, 16.5],
  [47.78405, 13.18210, 18.0],
  [47.78412, 13.18300, 20.0],
  [47.78415, 13.18400, 22.0],   // Sector 1 line
  [47.78413, 13.18490, 24.0],
  // Right turn at end of back straight
  [47.78405, 13.18545, 26.0],
  [47.78390, 13.18580, 27.5],
  [47.78370, 13.18595, 29.0],   // apex
  [47.78350, 13.18580, 30.5],
  // S-curve section (middle)
  [47.78330, 13.18555, 32.0],
  [47.78310, 13.18535, 33.5],
  [47.78290, 13.18520, 35.0],
  [47.78270, 13.18510, 36.5],   // Sector 2 line
  [47.78255, 13.18490, 38.0],
  [47.78245, 13.18455, 40.0],   // left turn
  [47.78248, 13.18415, 42.0],
  // Right-left chicane heading north-west
  [47.78258, 13.18375, 44.0],
  [47.78272, 13.18342, 46.0],
  [47.78290, 13.18325, 48.0],   // Sector 3 line
  [47.78308, 13.18330, 50.0],
  // Final chicane + approach to S/F
  [47.78318, 13.18360, 52.0],
  [47.78325, 13.18400, 54.5],
  [47.78328, 13.18440, 56.5],
  [47.78330, 13.18460, 58.0],   // back to S/F (base lap = 58s, actual ~95s)
];

// Sector trigger positions (which waypoint index approximates each sector line)
const SECTOR1_IDX = 13;   // after back straight entry
const SECTOR2_IDX = 23;   // mid-S-section
const SECTOR3_IDX = 28;   // before final chicane

// Rescale times to a realistic lap time
function scaleLap(waypoints, targetMs, noiseScale = 0.0003) {
  const rawEnd = waypoints[waypoints.length - 1][2];
  return waypoints.map(([lat, lon, t]) => {
    const scaledMs = Math.round((t / rawEnd) * targetMs);
    // tiny GPS noise
    const noise = () => (Math.random() - 0.5) * noiseScale;
    return [
      parseFloat((lat + noise()).toFixed(7)),
      parseFloat((lon + noise()).toFixed(7)),
      scaledMs,
    ];
  });
}

function interpolateWaypoints(wps, pointsPerSegment = 6) {
  const result = [];
  for (let i = 0; i < wps.length - 1; i++) {
    const [lat1, lon1, t1] = wps[i];
    const [lat2, lon2, t2] = wps[i + 1];
    for (let j = 0; j < pointsPerSegment; j++) {
      const f = j / pointsPerSegment;
      result.push([
        parseFloat((lat1 + (lat2 - lat1) * f).toFixed(7)),
        parseFloat((lon1 + (lon2 - lon1) * f).toFixed(7)),
        Math.round(t1 + (t2 - t1) * f),
      ]);
    }
  }
  result.push(wps[wps.length - 1].slice(0, 3));
  return result;
}

function sectorTimes(lapMs) {
  // S1 ≈ 42%, S2 ≈ 32%, S3 ≈ 26%  (rough Salzburgring split)
  const s1 = Math.round(lapMs * 0.420 + (Math.random() - 0.5) * 400);
  const s2 = Math.round(lapMs * 0.320 + (Math.random() - 0.5) * 300);
  const s3 = lapMs - s1 - s2;
  return [s1, s2, s3];
}

// Base lap times for each session / lap (in ms)
const SESSION_DATA = [
  {
    id: 'sess_A1B2C3D4',
    laps: [96450, 95120, 94780, 95890, 95340],
    label: 'Session 1 – Morgen',
  },
  {
    id: 'sess_E5F6G7H8',
    laps: [97100, 95600, 94200, 93850, 94550],
    label: 'Session 2 – Nachmittag',
  },
];

// ── Track file ────────────────────────────────────────────────────────────────
const trackJson = {
  name: 'Salzburgring',
  country: 'Österreich',
  length_km: 4.241,
  is_circuit: true,
  sf: [47.78340, 13.18460, 47.78320, 13.18460],  // perpendicular S/F points
  sectors: [
    { lat: 47.78415, lon: 13.18400, name: 'S1' },
    { lat: 47.78270, lon: 13.18510, name: 'S2' },
    { lat: 47.78290, lon: 13.18325, name: 'S3' },
  ],
};

// ── Generate sessions ─────────────────────────────────────────────────────────
function buildSession(tmpl) {
  const laps = tmpl.laps.map((totalMs, i) => {
    const scaledWps = scaleLap(WAYPOINTS, totalMs);
    const denseWps  = interpolateWaypoints(scaledWps, 5);
    // The ESP32 saves every 5th point — simulate that here too
    const trackPoints = denseWps.filter((_, idx) => idx % 5 === 0);
    return {
      lap: i + 1,
      total_ms: totalMs,
      sectors: sectorTimes(totalMs),
      track_points: trackPoints,
    };
  });

  return {
    id: tmpl.id,
    track: 'Salzburgring',
    laps,
  };
}

// ── Write files ───────────────────────────────────────────────────────────────
const outDir = path.join(__dirname, 'sd_card_data');
fs.mkdirSync(path.join(outDir, 'tracks'),   { recursive: true });
fs.mkdirSync(path.join(outDir, 'sessions'), { recursive: true });

// Track file
fs.writeFileSync(
  path.join(outDir, 'tracks', 'Salzburgring.json'),
  JSON.stringify(trackJson),
);
console.log('✓  tracks/Salzburgring.json');

// Session files
for (const tmpl of SESSION_DATA) {
  const sess = buildSession(tmpl);
  fs.writeFileSync(
    path.join(outDir, 'sessions', `${tmpl.id}.json`),
    JSON.stringify(sess),
  );
  const bestMs = Math.min(...tmpl.laps);
  const m = Math.floor(bestMs / 60000);
  const s = ((bestMs % 60000) / 1000).toFixed(3);
  console.log(`✓  sessions/${tmpl.id}.json  (${tmpl.laps.length} Runden, Bestzeit ${m}:${s.padStart(6,'0')})`);
}

console.log(`\nFertig! Inhalt von '${outDir}' auf die SD-Karte kopieren.`);
console.log('Ordnerstruktur auf SD:');
console.log('  /tracks/Salzburgring.json');
console.log('  /sessions/sess_A1B2C3D4.json');
console.log('  /sessions/sess_E5F6G7H8.json');
