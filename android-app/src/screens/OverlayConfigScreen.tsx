/**
 * OverlayConfigScreen — user customizes the video HUD.
 *
 * - Toggle each element (speed / lap time / delta / G-meter / track name)
 * - Pick corner position for the big ones
 * - Font size slider
 * - Backdrop opacity slider
 * - Live preview at the top using a sample LapChannels
 */

import React, { useEffect, useState } from 'react';
import {
  View, Text, StyleSheet, ScrollView, TouchableOpacity, Switch,
  useWindowDimensions,
} from 'react-native';
import { C } from '../theme';
import {
  OverlayConfig, Corner, DEFAULT_OVERLAY_CONFIG,
  loadOverlayConfig, saveOverlayConfig,
} from '../overlayConfig';
import VideoOverlay from '../components/VideoOverlay';
import { LapChannels } from '../analysis';

// Inline fallback slider — avoids a native dep. Simple plus/minus buttons.
function StepSlider({ value, min, max, step, onChange, fmt }: {
  value: number; min: number; max: number; step: number;
  onChange: (v: number) => void; fmt: (v: number) => string;
}) {
  return (
    <View style={s.stepRow}>
      <TouchableOpacity
        style={s.stepBtn}
        onPress={() => onChange(Math.max(min, +(value - step).toFixed(2)))}
      ><Text style={s.stepBtnTxt}>−</Text></TouchableOpacity>
      <Text style={s.stepVal}>{fmt(value)}</Text>
      <TouchableOpacity
        style={s.stepBtn}
        onPress={() => onChange(Math.min(max, +(value + step).toFixed(2)))}
      ><Text style={s.stepBtnTxt}>+</Text></TouchableOpacity>
    </View>
  );
}

const CORNER_LABEL: Record<Corner, string> = {
  tl: 'Oben links', tr: 'Oben rechts',
  bl: 'Unten links', br: 'Unten rechts',
};

function CornerPicker({ value, onChange }: { value: Corner; onChange: (c: Corner) => void }) {
  return (
    <View style={s.cornerRow}>
      {(['tl', 'tr', 'bl', 'br'] as Corner[]).map(c => (
        <TouchableOpacity
          key={c}
          style={[s.cornerBtn, value === c && s.cornerBtnActive]}
          onPress={() => onChange(c)}
        >
          <Text style={[s.cornerBtnTxt, value === c && { color: '#000' }]}>
            {CORNER_LABEL[c]}
          </Text>
        </TouchableOpacity>
      ))}
    </View>
  );
}

/** Synthetic channel data for live preview (3 laps of simulated values). */
function samplePreviewChannels(): LapChannels {
  const n = 60;
  const t_ms: number[]     = [];
  const dist_m: number[]   = [];
  const speed_kmh: number[]= [];
  const g_long: number[]   = [];
  const g_lat: number[]    = [];
  const heading: number[]  = [];
  for (let i = 0; i < n; i++) {
    t_ms.push(i * 1000);
    dist_m.push(i * 50);
    const phase = i / n;
    speed_kmh.push(80 + 50 * Math.sin(phase * Math.PI * 2));
    g_lat.push(1.2 * Math.sin(phase * Math.PI * 4));
    g_long.push(0.8 * Math.cos(phase * Math.PI * 4));
    heading.push(phase * 360);
  }
  return { t_ms, dist_m, speed_kmh, g_long, g_lat, heading };
}

export default function OverlayConfigScreen() {
  const { width: winW } = useWindowDimensions();
  const previewW = winW;
  const previewH = Math.round(winW * 9 / 16);

  const [cfg, setCfg] = useState<OverlayConfig>(DEFAULT_OVERLAY_CONFIG);
  const [time, setTime] = useState(15000);
  const preview = React.useMemo(samplePreviewChannels, []);

  useEffect(() => {
    loadOverlayConfig().then(setCfg);
  }, []);

  // Animate preview cursor for demo
  useEffect(() => {
    const id = setInterval(() => setTime(t => (t + 500) % 59000), 250);
    return () => clearInterval(id);
  }, []);

  function update<K extends keyof OverlayConfig>(key: K, val: OverlayConfig[K]) {
    const next = { ...cfg, [key]: val };
    setCfg(next);
    saveOverlayConfig(next);  // persist on every change — no Save button needed
  }

  return (
    <ScrollView style={s.root} contentContainerStyle={{ paddingBottom: 40 }}>
      {/* Live preview */}
      <View style={[s.preview, { width: previewW, height: previewH }]}>
        <View style={s.previewBg}>
          <Text style={s.previewHint}>Vorschau</Text>
        </View>
        <VideoOverlay
          width={previewW}
          height={previewH}
          timeMs={time}
          channels={preview}
          delta={preview.t_ms.map((t, i) => (i - 30) * 100)}
          lapNumber={3}
          trackName="BRL Testtrack"
          config={cfg}
        />
      </View>

      <View style={s.body}>
        <Text style={s.section}>Elemente</Text>
        <ToggleRow label="Geschwindigkeit" value={cfg.showSpeed}   onChange={v => update('showSpeed', v)} />
        <ToggleRow label="Rundenzeit"      value={cfg.showLapTime} onChange={v => update('showLapTime', v)} />
        <ToggleRow label="Delta"           value={cfg.showDelta}   onChange={v => update('showDelta', v)} />
        <ToggleRow label="G-Meter"         value={cfg.showGMeter}  onChange={v => update('showGMeter', v)} />
        <ToggleRow label="Strecke anzeigen" value={cfg.showTrackName} onChange={v => update('showTrackName', v)} />

        <Text style={s.section}>Position Geschwindigkeit</Text>
        <CornerPicker value={cfg.positionSpeed} onChange={v => update('positionSpeed', v)} />

        <Text style={s.section}>Position Rundenzeit</Text>
        <CornerPicker value={cfg.positionLapTime} onChange={v => update('positionLapTime', v)} />

        <Text style={s.section}>Position G-Meter</Text>
        <CornerPicker value={cfg.positionGMeter} onChange={v => update('positionGMeter', v)} />

        <Text style={s.section}>Schriftgröße</Text>
        <StepSlider
          value={cfg.fontScale} min={0.7} max={1.6} step={0.1}
          onChange={v => update('fontScale', v)}
          fmt={v => `${Math.round(v * 100)} %`}
        />

        <Text style={s.section}>Hintergrund-Deckkraft</Text>
        <StepSlider
          value={cfg.backdropAlpha} min={0} max={255} step={15}
          onChange={v => update('backdropAlpha', Math.round(v))}
          fmt={v => `${Math.round((v / 255) * 100)} %`}
        />

        <TouchableOpacity
          style={s.resetBtn}
          onPress={() => { setCfg(DEFAULT_OVERLAY_CONFIG); saveOverlayConfig(DEFAULT_OVERLAY_CONFIG); }}
        >
          <Text style={s.resetBtnTxt}>Zurücksetzen</Text>
        </TouchableOpacity>
      </View>
    </ScrollView>
  );
}

function ToggleRow({ label, value, onChange }: {
  label: string; value: boolean; onChange: (v: boolean) => void;
}) {
  return (
    <View style={s.toggleRow}>
      <Text style={s.toggleLbl}>{label}</Text>
      <Switch
        value={value}
        onValueChange={onChange}
        trackColor={{ true: C.accent, false: C.surface2 }}
        thumbColor="#fff"
      />
    </View>
  );
}

const s = StyleSheet.create({
  root:        { flex: 1, backgroundColor: C.bg },
  preview:     { backgroundColor: '#222', position: 'relative' },
  previewBg:   { ...StyleSheet.absoluteFillObject, backgroundColor: '#1a1a22',
                 alignItems: 'center', justifyContent: 'center' },
  previewHint: { color: '#555', fontSize: 14, fontStyle: 'italic' },

  body:        { padding: 16 },
  section:     { color: C.dim, fontSize: 11, fontWeight: '700',
                 textTransform: 'uppercase', marginTop: 20, marginBottom: 8 },

  toggleRow:   { flexDirection: 'row', alignItems: 'center',
                 justifyContent: 'space-between', paddingVertical: 8 },
  toggleLbl:   { color: C.text, fontSize: 15 },

  cornerRow:   { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  cornerBtn:   { paddingHorizontal: 12, paddingVertical: 8, borderRadius: 6,
                 backgroundColor: C.surface2, minWidth: '48%' },
  cornerBtnActive: { backgroundColor: C.accent },
  cornerBtnTxt:{ color: C.dim, fontWeight: '600', fontSize: 13, textAlign: 'center' },

  stepRow:     { flexDirection: 'row', alignItems: 'center', justifyContent: 'center' },
  stepBtn:     { width: 48, height: 40, backgroundColor: C.accent, borderRadius: 6,
                 alignItems: 'center', justifyContent: 'center' },
  stepBtnTxt:  { color: '#000', fontSize: 22, fontWeight: '700' },
  stepVal:     { color: C.text, fontSize: 16, fontWeight: '700',
                 marginHorizontal: 20, minWidth: 80, textAlign: 'center' },

  resetBtn:    { marginTop: 30, padding: 12, borderRadius: 8,
                 borderWidth: 1, borderColor: C.danger, alignItems: 'center' },
  resetBtnTxt: { color: C.danger, fontWeight: '700' },
});
