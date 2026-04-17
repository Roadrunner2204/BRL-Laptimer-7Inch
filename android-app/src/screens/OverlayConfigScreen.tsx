/**
 * OverlayConfigScreen — free-positioning HUD editor.
 *
 * - Drag any visible widget directly on the video preview (finger-follow).
 * - Per-widget: visibility toggle + horizontal-anchor picker (start/center/end).
 * - Global: font scale, accent color (reserved), backdrop opacity.
 * - Saves on every change so no explicit Save button is needed. Drag moves
 *   update local state in real time but only persist on gesture release to
 *   avoid hammering AsyncStorage.
 */

import React, { useEffect, useRef, useState } from 'react';
import {
  View, Text, StyleSheet, ScrollView, TouchableOpacity, Switch,
  useWindowDimensions, PanResponder, Modal, Alert,
} from 'react-native';
import { C } from '../theme';
import {
  OverlayConfig, DEFAULT_OVERLAY_CONFIG, Widget, WidgetAnchor, WidgetType,
  WIDGET_COLOR_SWATCHES,
  loadOverlayConfig, saveOverlayConfig,
} from '../overlayConfig';
import VideoOverlay, { WIDGET_SIZES } from '../components/VideoOverlay';
import { LapChannels } from '../analysis';

const WIDGET_LABEL: Record<WidgetType, string> = {
  speed:      'Geschwindigkeit',
  lapTime:    'Rundenzeit',
  delta:      'Delta',
  gMeter:     'G-Meter',
  trackName:  'Streckenname',
  speedBar:   'Speed-Balken',
  gForce:     'G-Kraft gesamt',
  lapNumber:  'Rundennummer',
  miniMap:    'Mini-Karte',
};

// Every type the "Hinzufügen" button can instantiate. Multiple instances
// of the same type are allowed — each gets a unique id via Date.now().
const ADDABLE_TYPES: WidgetType[] = [
  'speed', 'lapTime', 'delta', 'gMeter', 'trackName',
  'speedBar', 'gForce', 'lapNumber', 'miniMap',
];

const ANCHOR_LABEL: Record<WidgetAnchor, string> = {
  start:  '◧ links',
  center: '◨ mitte',
  end:    '◨ rechts',
};

function leftOf(anchorPx: number, w: number, a: WidgetAnchor): number {
  return a === 'start' ? anchorPx
       : a === 'end'   ? anchorPx - w
       :                 anchorPx - w / 2;
}

/** Synthetic channel data for a static preview frame. */
function samplePreviewChannels(): LapChannels {
  const n = 60;
  const t_ms: number[] = [], dist_m: number[] = [], speed_kmh: number[] = [];
  const g_long: number[] = [], g_lat: number[] = [], heading: number[] = [];
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

// ── Draggable handle overlay ──────────────────────────────────────────
// A transparent View positioned exactly over the widget's visual extent.
// PanResponder captures touch + drag; we translate finger delta to widget
// (x, y) updates. Uses a "latest-ref" so the PanResponder closures always
// see fresh state/callbacks without being re-created.

interface DragHandleProps {
  widget: Widget;
  previewW: number;
  previewH: number;
  fs: number;
  selected: boolean;
  onSelect: (id: string) => void;
  onMove: (id: string, x: number, y: number) => void;   // live updates
  onCommit: () => void;                                  // persist on release
}

function DragHandle(props: DragHandleProps) {
  const latest = useRef(props);
  latest.current = props;

  // Start position captured on grant so move deltas are relative to the
  // widget's original anchor, not whatever is currently rendered.
  const startX = useRef(0);
  const startY = useRef(0);

  const pan = useRef(PanResponder.create({
    onStartShouldSetPanResponder: () => true,
    onMoveShouldSetPanResponder:  (_, g) => Math.abs(g.dx) + Math.abs(g.dy) > 2,
    onPanResponderGrant: () => {
      startX.current = latest.current.widget.x;
      startY.current = latest.current.widget.y;
      latest.current.onSelect(latest.current.widget.id);
    },
    onPanResponderMove: (_, g) => {
      const { widget, previewW, previewH, onMove } = latest.current;
      const dxFrac = g.dx / previewW;
      const dyFrac = g.dy / previewH;
      const nx = Math.max(0, Math.min(1, startX.current + dxFrac));
      const ny = Math.max(0, Math.min(1, startY.current + dyFrac));
      onMove(widget.id, nx, ny);
    },
    onPanResponderRelease: () => { latest.current.onCommit(); },
    onPanResponderTerminate: () => { latest.current.onCommit(); },
  })).current;

  const { widget, previewW, previewH, fs, selected } = props;
  const size = WIDGET_SIZES[widget.type];
  const w = size.w * fs;
  const h = size.h * fs;
  const left = leftOf(widget.x * previewW, w, widget.anchor);
  const top  = widget.y * previewH;

  return (
    <View
      style={[s.dragHandle, {
        left, top, width: w, height: h,
        borderColor: selected ? C.accent : 'rgba(255,255,255,0.35)',
        borderWidth: selected ? 2 : 1,
      }]}
      {...pan.panHandlers}
    />
  );
}

// ── Step slider (avoids a native dep) ────────────────────────────────
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

// ── Main screen ───────────────────────────────────────────────────────
export default function OverlayConfigScreen() {
  const { width: winW } = useWindowDimensions();
  const previewW = winW;
  const previewH = Math.round(winW * 9 / 16);

  const [cfg, setCfg] = useState<OverlayConfig>(DEFAULT_OVERLAY_CONFIG);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [showAddModal, setShowAddModal] = useState(false);
  const [time, setTime] = useState(15000);
  const preview = React.useMemo(samplePreviewChannels, []);

  useEffect(() => { loadOverlayConfig().then(setCfg); }, []);

  // Animate preview cursor so widget values aren't frozen in the editor.
  useEffect(() => {
    const id = setInterval(() => setTime(t => (t + 500) % 59000), 250);
    return () => clearInterval(id);
  }, []);

  // Latest cfg ref so the move callback reads the up-to-date state in its
  // throttled closure without rebuilding the PanResponder per render.
  const latestCfg = useRef(cfg);
  latestCfg.current = cfg;

  function updateCfg(next: OverlayConfig, persist = true) {
    setCfg(next);
    if (persist) saveOverlayConfig(next);
  }

  // Drag live — update state only, persist on commit.
  function handleMove(id: string, x: number, y: number) {
    const widgets = latestCfg.current.widgets.map(w =>
      w.id === id ? { ...w, x, y } : w);
    setCfg({ ...latestCfg.current, widgets });
  }
  function handleCommit() { saveOverlayConfig(latestCfg.current); }

  function updateWidget(id: string, patch: Partial<Widget>) {
    const widgets = cfg.widgets.map(w => w.id === id ? { ...w, ...patch } : w);
    updateCfg({ ...cfg, widgets });
  }

  // Instantiate a new widget in the middle of the frame. User can drag it
  // to the desired position afterward. Multiple instances of the same type
  // are allowed — each gets a unique id based on timestamp.
  function addWidget(type: WidgetType) {
    const id = `${type}_${Date.now()}`;
    const fresh: Widget = {
      id, type, x: 0.5, y: 0.5, anchor: 'center', visible: true,
    };
    updateCfg({ ...cfg, widgets: [...cfg.widgets, fresh] });
    setSelectedId(id);
    setShowAddModal(false);
  }

  function deleteWidget(id: string) {
    Alert.alert(
      'Widget entfernen?',
      'Das Widget wird aus dem Overlay entfernt. Die Einstellung kann jederzeit durch „Hinzufügen" wiederhergestellt werden.',
      [
        { text: 'Abbrechen', style: 'cancel' },
        { text: 'Entfernen', style: 'destructive', onPress: () => {
          updateCfg({ ...cfg, widgets: cfg.widgets.filter(w => w.id !== id) });
          if (selectedId === id) setSelectedId(null);
        }},
      ]);
  }

  return (
    <ScrollView style={s.root} contentContainerStyle={{ paddingBottom: 40 }}>
      {/* Preview + drag handles */}
      <View style={[s.preview, { width: previewW, height: previewH }]}>
        <View style={s.previewBg}>
          <Text style={s.previewHint}>Widget ziehen zum Positionieren</Text>
        </View>
        <VideoOverlay
          width={previewW}
          height={previewH}
          timeMs={time}
          channels={preview}
          delta={preview.t_ms.map((_, i) => (i - 30) * 100)}
          lapNumber={3}
          trackName="BRL Testtrack"
          config={cfg}
        />
        {cfg.widgets.filter(w => w.visible).map(w => (
          <DragHandle
            key={w.id}
            widget={w}
            previewW={previewW}
            previewH={previewH}
            fs={cfg.fontScale}
            selected={selectedId === w.id}
            onSelect={setSelectedId}
            onMove={handleMove}
            onCommit={handleCommit}
          />
        ))}
      </View>

      <View style={s.body}>
        <View style={s.sectionRow}>
          <Text style={s.section}>Widgets</Text>
          <TouchableOpacity style={s.addBtn} onPress={() => setShowAddModal(true)}>
            <Text style={s.addBtnTxt}>+  Hinzufügen</Text>
          </TouchableOpacity>
        </View>
        {cfg.widgets.map(w => {
          const isSel = selectedId === w.id;
          return (
            <View key={w.id}
                  style={[s.widgetCard, isSel && s.widgetCardSel]}>
              <View style={s.widgetHead}>
                <TouchableOpacity style={{ flex: 1 }}
                                  onPress={() => setSelectedId(isSel ? null : w.id)}>
                  <Text style={s.widgetName}>
                    {WIDGET_LABEL[w.type]} {isSel ? '▾' : '▸'}
                  </Text>
                  <Text style={s.widgetPos}>
                    x {Math.round(w.x * 100)} %  ·  y {Math.round(w.y * 100)} %
                    {w.scale != null && w.scale !== 1 &&
                      `  ·  ${Math.round(w.scale * 100)} %`}
                  </Text>
                </TouchableOpacity>
                <Switch
                  value={w.visible}
                  onValueChange={v => updateWidget(w.id, { visible: v })}
                  trackColor={{ true: C.accent, false: C.surface2 }}
                  thumbColor="#fff"
                />
              </View>

              {/* Always-visible: anchor picker (determines which way the
                  widget extends from its drag anchor point). */}
              <View style={s.anchorRow}>
                {(['start', 'center', 'end'] as WidgetAnchor[]).map(a => (
                  <TouchableOpacity
                    key={a}
                    style={[s.anchorBtn, w.anchor === a && s.anchorBtnActive]}
                    onPress={() => updateWidget(w.id, { anchor: a })}
                  >
                    <Text style={[s.anchorBtnTxt, w.anchor === a && { color: '#000' }]}>
                      {ANCHOR_LABEL[a]}
                    </Text>
                  </TouchableOpacity>
                ))}
              </View>

              {/* Detail panel only when this widget is selected/expanded —
                  keeps the list short when you have many widgets. */}
              {isSel && (
                <View style={s.detailBox}>
                  <Text style={s.detailLabel}>Größe</Text>
                  <StepSlider
                    value={w.scale ?? 1.0} min={0.5} max={2.5} step={0.1}
                    onChange={v => updateWidget(w.id, { scale: v })}
                    fmt={v => `${Math.round(v * 100)} %`}
                  />
                  <Text style={s.detailLabel}>Farbe</Text>
                  <ScrollView horizontal showsHorizontalScrollIndicator={false}
                              contentContainerStyle={{ paddingRight: 10 }}>
                    {WIDGET_COLOR_SWATCHES.map(sw => {
                      const active = (w.color ?? null) === sw.value;
                      return (
                        <TouchableOpacity
                          key={sw.name}
                          style={[s.swatch,
                                  active && s.swatchActive,
                                  sw.value
                                    ? { backgroundColor: sw.value }
                                    : { backgroundColor: C.surface2,
                                        borderWidth: 1, borderColor: C.border }]}
                          onPress={() => updateWidget(w.id,
                            { color: sw.value === null ? undefined : sw.value })}
                        >
                          {sw.value === null && (
                            <Text style={s.swatchAutoTxt}>Auto</Text>
                          )}
                        </TouchableOpacity>
                      );
                    })}
                  </ScrollView>

                  <TouchableOpacity style={s.delWidgetBtn}
                                    onPress={() => deleteWidget(w.id)}>
                    <Text style={s.delWidgetTxt}>Widget entfernen</Text>
                  </TouchableOpacity>
                </View>
              )}
            </View>
          );
        })}

        <Text style={s.section}>Schriftgröße</Text>
        <StepSlider
          value={cfg.fontScale} min={0.6} max={1.8} step={0.1}
          onChange={v => updateCfg({ ...cfg, fontScale: v })}
          fmt={v => `${Math.round(v * 100)} %`}
        />

        <Text style={s.section}>Hintergrund-Deckkraft</Text>
        <StepSlider
          value={cfg.backdropAlpha} min={0} max={255} step={15}
          onChange={v => updateCfg({ ...cfg, backdropAlpha: Math.round(v) })}
          fmt={v => `${Math.round((v / 255) * 100)} %`}
        />

        <TouchableOpacity
          style={s.resetBtn}
          onPress={() => updateCfg(DEFAULT_OVERLAY_CONFIG)}
        >
          <Text style={s.resetBtnTxt}>Zurücksetzen</Text>
        </TouchableOpacity>
      </View>

      {/* Add-widget picker modal — opens from the "+ Hinzufügen" button. */}
      <Modal visible={showAddModal} transparent animationType="fade"
             onRequestClose={() => setShowAddModal(false)}>
        <TouchableOpacity style={s.modalBackdrop} activeOpacity={1}
                          onPress={() => setShowAddModal(false)}>
          <TouchableOpacity style={s.modalCard} activeOpacity={1}>
            <Text style={s.modalTitle}>Widget hinzufügen</Text>
            {ADDABLE_TYPES.map(t => (
              <TouchableOpacity key={t} style={s.modalRow}
                                onPress={() => addWidget(t)}>
                <Text style={s.modalRowTxt}>{WIDGET_LABEL[t]}</Text>
              </TouchableOpacity>
            ))}
            <TouchableOpacity style={s.modalCancel}
                              onPress={() => setShowAddModal(false)}>
              <Text style={s.modalCancelTxt}>Abbrechen</Text>
            </TouchableOpacity>
          </TouchableOpacity>
        </TouchableOpacity>
      </Modal>
    </ScrollView>
  );
}

const s = StyleSheet.create({
  root:        { flex: 1, backgroundColor: C.bg },
  preview:     { backgroundColor: '#222', position: 'relative' },
  previewBg:   { ...StyleSheet.absoluteFillObject, backgroundColor: '#1a1a22',
                 alignItems: 'center', justifyContent: 'center' },
  previewHint: { color: '#555', fontSize: 13, fontStyle: 'italic' },

  dragHandle:  { position: 'absolute', borderRadius: 4,
                 backgroundColor: 'transparent' },

  body:        { padding: 16 },
  section:     { color: C.dim, fontSize: 11, fontWeight: '700',
                 textTransform: 'uppercase', marginTop: 20, marginBottom: 8 },

  widgetCard:  { backgroundColor: C.surface, borderRadius: 10, padding: 12,
                 marginBottom: 8, borderWidth: 1, borderColor: 'transparent' },
  widgetCardSel:{ borderColor: C.accent },
  widgetHead:  { flexDirection: 'row', alignItems: 'center' },
  widgetName:  { color: C.text, fontSize: 15, fontWeight: '700' },
  widgetPos:   { color: C.dim, fontSize: 11, marginTop: 2,
                 fontVariant: ['tabular-nums'] },

  anchorRow:   { flexDirection: 'row', marginTop: 10, gap: 6 },
  anchorBtn:   { flex: 1, paddingVertical: 8, borderRadius: 6,
                 backgroundColor: C.surface2, alignItems: 'center' },
  anchorBtnActive:{ backgroundColor: C.accent },
  anchorBtnTxt:{ color: C.dim, fontWeight: '600', fontSize: 12 },

  detailBox:   { marginTop: 12, paddingTop: 12,
                 borderTopWidth: 1, borderTopColor: C.border },
  detailLabel: { color: C.dim, fontSize: 11, fontWeight: '700',
                 textTransform: 'uppercase', marginBottom: 8, marginTop: 4 },
  swatch:      { width: 36, height: 36, borderRadius: 18, marginRight: 8,
                 alignItems: 'center', justifyContent: 'center' },
  swatchActive:{ borderWidth: 3, borderColor: '#fff' },
  swatchAutoTxt:{ color: C.dim, fontSize: 9, fontWeight: '700' },

  stepRow:     { flexDirection: 'row', alignItems: 'center', justifyContent: 'center' },
  stepBtn:     { width: 48, height: 40, backgroundColor: C.accent, borderRadius: 6,
                 alignItems: 'center', justifyContent: 'center' },
  stepBtnTxt:  { color: '#000', fontSize: 22, fontWeight: '700' },
  stepVal:     { color: C.text, fontSize: 16, fontWeight: '700',
                 marginHorizontal: 20, minWidth: 80, textAlign: 'center' },

  resetBtn:    { marginTop: 30, padding: 12, borderRadius: 8,
                 borderWidth: 1, borderColor: C.danger, alignItems: 'center' },
  resetBtnTxt: { color: C.danger, fontWeight: '700' },

  sectionRow:  { flexDirection: 'row', alignItems: 'center',
                 justifyContent: 'space-between' },
  addBtn:      { paddingHorizontal: 12, paddingVertical: 6, borderRadius: 6,
                 backgroundColor: C.accent },
  addBtnTxt:   { color: '#000', fontSize: 12, fontWeight: '700' },

  delWidgetBtn:{ marginTop: 14, paddingVertical: 10, borderRadius: 6,
                 borderWidth: 1, borderColor: C.danger, alignItems: 'center' },
  delWidgetTxt:{ color: C.danger, fontSize: 12, fontWeight: '700' },

  modalBackdrop:{ flex: 1, backgroundColor: 'rgba(0,0,0,0.75)',
                  justifyContent: 'center', alignItems: 'center',
                  padding: 24 },
  modalCard:   { width: '100%', maxWidth: 360, backgroundColor: C.surface,
                 borderRadius: 12, padding: 16 },
  modalTitle:  { color: C.text, fontSize: 17, fontWeight: '700',
                 marginBottom: 14 },
  modalRow:    { paddingVertical: 12, borderBottomWidth: 1,
                 borderBottomColor: C.border },
  modalRowTxt: { color: C.text, fontSize: 15, fontWeight: '600' },
  modalCancel: { marginTop: 14, padding: 10, alignItems: 'center' },
  modalCancelTxt:{ color: C.dim, fontSize: 13, fontWeight: '700' },
});
