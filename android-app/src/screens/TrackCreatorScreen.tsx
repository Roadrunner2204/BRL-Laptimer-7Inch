/**
 * TrackCreatorScreen — build a Track by tapping points on an OpenStreetMap.
 *
 * Workflow:
 *  1. Map centers on the phone's current GPS position (expo-location).
 *  2. User selects a placement mode (S/F 1, S/F 2, Ziel 1, Ziel 2, +Sektor).
 *  3. Taps on the map set that point. Markers/lines update live.
 *  4. Save sends the Track JSON to the device over WiFi.
 *
 * A track = start/finish line (2 lat/lon points) + optional finish line
 * (A-B stage) + 0..8 sector crossing points. Matches TrackDef in
 * main/data/track_db.h.
 */

import React, { useEffect, useMemo, useRef, useState } from 'react';
import {
  View, Text, TextInput, ScrollView, TouchableOpacity, StyleSheet,
  Alert, KeyboardAvoidingView, Platform, ActivityIndicator,
} from 'react-native';
import { WebView, WebViewMessageEvent } from 'react-native-webview';
import * as Location from 'expo-location';
import { NativeStackNavigationProp } from '@react-navigation/native-stack';
import { RouteProp } from '@react-navigation/native';
import { RootStackParamList } from '../App';
import { C } from '../theme';
import { Track, SectorDef, SectorLineDef } from '../types';
import { postTrack } from '../api';
import { saveLocalTrack } from '../storage';
import { preferCellularDefault, unbindDefault } from '../network';

type Props = {
  navigation: NativeStackNavigationProp<RootStackParamList, 'TrackCreator'>;
  route: RouteProp<RootStackParamList, 'TrackCreator'>;
};

const MAX_SECTORS = 8;

type LatLon = { lat: number; lon: number };
type PlaceMode = null | 'sf1' | 'sf2' | 'fin1' | 'fin2' | 'sector';

function fmtCoord(v: number | undefined): string {
  if (v == null || !Number.isFinite(v)) return '—';
  return v.toFixed(5);
}

export default function TrackCreatorScreen({ navigation, route }: Props) {
  const initial = route.params?.initial;
  const editing = !!initial;

  const [name, setName]         = useState(initial?.name ?? '');
  const [country, setCountry]   = useState(initial?.country ?? 'DE');
  const [lengthKm, setLengthKm] = useState(
    initial && initial.length_km ? String(initial.length_km) : '');
  const [isCircuit, setIsCircuit] = useState(initial?.is_circuit ?? true);

  const [sf1, setSf1] = useState<LatLon | null>(
    initial ? { lat: initial.sf[0], lon: initial.sf[1] } : null);
  const [sf2, setSf2] = useState<LatLon | null>(
    initial ? { lat: initial.sf[2], lon: initial.sf[3] } : null);
  const [fin1, setFin1] = useState<LatLon | null>(
    initial?.fin ? { lat: initial.fin[0], lon: initial.fin[1] } : null);
  const [fin2, setFin2] = useState<LatLon | null>(
    initial?.fin ? { lat: initial.fin[2], lon: initial.fin[3] } : null);
  const [sectors, setSectors] = useState<LatLon[]>(
    (initial?.sectors ?? []).map((s: any) => ({
      lat: typeof s.lat1 === 'number' ? s.lat1 : s.lat,
      lon: typeof s.lon1 === 'number' ? s.lon1 : s.lon,
    })));

  const [mode, setMode] = useState<PlaceMode>(editing ? null : 'sf1');
  const [busy, setBusy] = useState(false);

  // Address search (Nominatim — free OSM geocoder; requires internet)
  const [searchQuery, setSearchQuery] = useState('');
  const [searchBusy, setSearchBusy] = useState(false);
  const [searchResults, setSearchResults] = useState<
    { display_name: string; lat: string; lon: string }[]
  >([]);

  const webviewRef = useRef<WebView>(null);
  const readyRef = useRef(false);

  // While this screen is open, bind the process default network to cellular
  // so OSM map tiles + Nominatim search can reach the internet — the
  // laptimer WiFi stays connected but display calls (postTrack etc.) go
  // through wifiFetch which explicitly targets the WiFi transport.
  useEffect(() => {
    preferCellularDefault();
    return () => { unbindDefault(); };
  }, []);

  // On mount: in edit-mode center on the track itself, otherwise on the
  // phone's current GPS position.
  useEffect(() => {
    if (editing && sf1) {
      // Delay slightly so the WebView onLoadEnd has fired
      const t = setTimeout(() => {
        postToMap({ type: 'center', lat: sf1.lat, lon: sf1.lon, zoom: 16 });
      }, 400);
      return () => clearTimeout(t);
    }
    (async () => {
      try {
        const { status } = await Location.requestForegroundPermissionsAsync();
        if (status !== 'granted') return;
        const pos = await Location.getCurrentPositionAsync({
          accuracy: Location.Accuracy.Balanced,
        });
        postToMap({
          type: 'center',
          lat: pos.coords.latitude,
          lon: pos.coords.longitude,
          zoom: 17,
        });
      } catch { /* ignore — map will use default center */ }
    })();
    return undefined;
  }, []);

  // Push marker state whenever any point changes
  useEffect(() => {
    postToMap({
      type: 'markers',
      sf1, sf2, fin1, fin2,
      sectors,
      showFin: !isCircuit,
      sfLabel: isCircuit ? 'S/F' : 'Start',
    });
  }, [sf1, sf2, fin1, fin2, sectors, isCircuit]);

  // Push mode so the WebView can show a targeting cursor
  useEffect(() => {
    postToMap({ type: 'mode', mode });
  }, [mode]);

  function postToMap(msg: any) {
    if (!readyRef.current) return;
    webviewRef.current?.injectJavaScript(
      `window.__handle(${JSON.stringify(JSON.stringify(msg))}); true;`
    );
  }

  function onMessage(e: WebViewMessageEvent) {
    try {
      const msg = JSON.parse(e.nativeEvent.data);
      if (msg.type === 'tap' && typeof msg.lat === 'number' && typeof msg.lon === 'number') {
        applyPlacement(msg.lat, msg.lon);
      } else if (msg.type === 'drag' && typeof msg.key === 'string'
                 && typeof msg.lat === 'number' && typeof msg.lon === 'number') {
        // User dragged an existing marker — update the matching state
        // directly without going through the placement-mode dance.
        const p = { lat: msg.lat, lon: msg.lon };
        switch (msg.key) {
          case 'sf1':  setSf1(p); break;
          case 'sf2':  setSf2(p); break;
          case 'fin1': setFin1(p); break;
          case 'fin2': setFin2(p); break;
          default:
            if (msg.key.startsWith('sector_')) {
              const i = parseInt(msg.key.slice(7), 10);
              if (!isNaN(i) && i >= 0 && i < sectors.length) {
                const next = [...sectors];
                next[i] = p;
                setSectors(next);
              }
            }
        }
      }
    } catch { /* ignore malformed msg */ }
  }

  function applyPlacement(lat: number, lon: number) {
    const p = { lat, lon };
    switch (mode) {
      case 'sf1':
        setSf1(p);
        setMode('sf2');
        break;
      case 'sf2':
        setSf2(p);
        setMode(isCircuit ? null : 'fin1');
        break;
      case 'fin1':
        setFin1(p);
        setMode('fin2');
        break;
      case 'fin2':
        setFin2(p);
        setMode(null);
        break;
      case 'sector':
        if (sectors.length < MAX_SECTORS) {
          setSectors([...sectors, p]);
          // stay in sector mode so user can place several in a row
        } else {
          setMode(null);
        }
        break;
      default:
        break;
    }
  }

  async function searchAddress() {
    const q = searchQuery.trim();
    if (q.length < 3) {
      Alert.alert('Suche', 'Mindestens 3 Zeichen eingeben.');
      return;
    }
    setSearchBusy(true);
    try {
      // Nominatim usage policy: identify via User-Agent, max 1 req/s.
      const url = `https://nominatim.openstreetmap.org/search?format=json&limit=5&q=${encodeURIComponent(q)}`;
      const r = await fetch(url, {
        headers: { 'User-Agent': 'BRL-Telemetry/1.0 (track-editor)' },
      });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const json = await r.json();
      if (!Array.isArray(json) || json.length === 0) {
        Alert.alert('Nicht gefunden',
          'Keine passende Adresse oder Ortsangabe.\nVersuche Stadt + Land, z. B. "Melk Österreich".');
        setSearchResults([]);
        return;
      }
      setSearchResults(json);
    } catch (e: any) {
      Alert.alert('Suche fehlgeschlagen',
        `Keine Internetverbindung oder Nominatim nicht erreichbar.\n\n${e?.message ?? String(e)}`);
    } finally {
      setSearchBusy(false);
    }
  }

  function applySearchResult(r: { lat: string; lon: string }) {
    const lat = parseFloat(r.lat);
    const lon = parseFloat(r.lon);
    if (!Number.isFinite(lat) || !Number.isFinite(lon)) return;
    postToMap({ type: 'center', lat, lon, zoom: 17 });
    setSearchResults([]);
  }

  async function useMyLocation() {
    try {
      const { status } = await Location.requestForegroundPermissionsAsync();
      if (status !== 'granted') {
        Alert.alert('Standort nicht erlaubt',
          'Bitte Standortfreigabe in den Einstellungen erlauben.');
        return;
      }
      const pos = await Location.getCurrentPositionAsync({
        accuracy: Location.Accuracy.High,
      });
      postToMap({
        type: 'center',
        lat: pos.coords.latitude,
        lon: pos.coords.longitude,
        zoom: 18,
      });
    } catch (e: any) {
      Alert.alert('Standort-Fehler', e?.message ?? String(e));
    }
  }

  function clearAll() {
    Alert.alert('Alle Punkte löschen?', 'Das setzt die komplette Strecken-Definition zurück.', [
      { text: 'Abbrechen', style: 'cancel' },
      { text: 'Löschen', style: 'destructive', onPress: () => {
        setSf1(null); setSf2(null);
        setFin1(null); setFin2(null);
        setSectors([]);
        setMode('sf1');
      }},
    ]);
  }

  function delSector(i: number) {
    setSectors(sectors.filter((_, j) => j !== i));
  }

  const sfComplete = !!sf1 && !!sf2;
  const finComplete = !!fin1 && !!fin2;
  const canSave = name.trim().length >= 2 && sfComplete && (isCircuit || finComplete);

  function buildTrack(): Track | null {
    if (!canSave) {
      Alert.alert('Unvollständig',
        'Name (≥2 Zeichen) und Start/Ziel-Linie sind Pflicht.' +
        (isCircuit ? '' : ' Ziel-Linie ebenfalls.'));
      return null;
    }
    return {
      name: name.trim(),
      country: country.trim() || 'Custom',
      length_km: Number.isFinite(parseFloat(lengthKm.replace(',', '.')))
        ? parseFloat(lengthKm.replace(',', '.')) : 0,
      is_circuit: isCircuit,
      sf: [sf1!.lat, sf1!.lon, sf2!.lat, sf2!.lon],
      ...(isCircuit ? {} : { fin: [fin1!.lat, fin1!.lon, fin2!.lat, fin2!.lon] as [number, number, number, number] }),
      sectors: sectors.map((p, i): SectorDef => ({ lat: p.lat, lon: p.lon, name: `S${i + 1}` })),
    };
  }

  // "Auf Gerät senden" — tries POST; on network failure, saves locally
  // with pending=true so the user can sync later from Home screen.
  async function saveToDevice() {
    const track = buildTrack();
    if (!track) return;
    setBusy(true);
    try {
      const res = await postTrack(track);
      Alert.alert('Strecke gespeichert',
        `"${res.name}" wurde auf das Gerät übertragen.\nGesamt: ${res.total} Strecken.`,
        [{ text: 'OK', onPress: () => navigation.goBack() }]);
    } catch (e: any) {
      // Fallback: save locally so the upload isn't lost.
      try {
        await saveLocalTrack(track, /*pending*/ true);
        Alert.alert('Gerät nicht erreichbar',
          `"${track.name}" wurde auf dem Handy gespeichert und wird automatisch hochgeladen, sobald du mit dem Laptimer verbunden bist.\n\nFehler: ${e?.message ?? String(e)}`,
          [{ text: 'OK', onPress: () => navigation.goBack() }]);
      } catch (se: any) {
        Alert.alert('Upload fehlgeschlagen',
          `${e?.message ?? String(e)}\nLokaler Speicherversuch: ${se?.message ?? String(se)}`);
      }
    } finally {
      setBusy(false);
    }
  }

  // "Lokal speichern" — explicit offline save, never tries the device.
  async function saveLocalOnly() {
    const track = buildTrack();
    if (!track) return;
    setBusy(true);
    try {
      await saveLocalTrack(track, /*pending*/ true);
      Alert.alert('Lokal gespeichert',
        `"${track.name}" liegt jetzt auf dem Handy. Sobald du mit dem Laptimer verbunden bist, kannst du sie vom Home-Bildschirm hochladen.`,
        [{ text: 'OK', onPress: () => navigation.goBack() }]);
    } catch (e: any) {
      Alert.alert('Speichern fehlgeschlagen', e?.message ?? String(e));
    } finally {
      setBusy(false);
    }
  }

  const modeHint = (() => {
    const startWord = isCircuit ? 'Start/Ziel' : 'Start';
    switch (mode) {
      case 'sf1':    return `Tippe Punkt 1 der ${startWord}-Linie`;
      case 'sf2':    return `Tippe Punkt 2 der ${startWord}-Linie`;
      case 'fin1':   return 'Tippe Punkt 1 der Ziel-Linie';
      case 'fin2':   return 'Tippe Punkt 2 der Ziel-Linie';
      case 'sector': return `Tippe Sektor ${sectors.length + 1} (max ${MAX_SECTORS})`;
      default:       return 'Wähle einen Modus und tippe auf die Karte';
    }
  })();

  return (
    <KeyboardAvoidingView
      style={{ flex: 1, backgroundColor: C.bg }}
      behavior={Platform.OS === 'ios' ? 'padding' : undefined}
    >
      {/* Map — fixed height, not inside scroll view so gestures work */}
      <View style={s.mapWrap}>
        <WebView
          ref={webviewRef}
          source={{ html: MAP_HTML }}
          style={s.map}
          javaScriptEnabled
          domStorageEnabled
          originWhitelist={['*']}
          mixedContentMode="always"
          onMessage={onMessage}
          onLoadEnd={() => {
            readyRef.current = true;
            // Replay state to the map now that the WebView is live.
            // (The mount-time effects fired before the HTML finished loading
            // and were silently dropped because ready was still false.)
            postToMap({
              type: 'markers',
              sf1, sf2, fin1, fin2, sectors,
              showFin: !isCircuit,
              sfLabel: isCircuit ? 'S/F' : 'Start',
            });
            postToMap({ type: 'mode', mode });
            if (editing && sf1) {
              postToMap({ type: 'center', lat: sf1.lat, lon: sf1.lon, zoom: 16 });
            }
          }}
          androidLayerType="hardware"
        />
        <View style={s.mapOverlay} pointerEvents="box-none">
          {/* Top: address search + mode hint */}
          <View pointerEvents="box-none">
            <View style={s.searchBox} pointerEvents="auto">
              <TextInput
                style={s.searchInput}
                placeholder="Adresse oder Ort suchen…"
                placeholderTextColor={C.dim}
                value={searchQuery}
                onChangeText={setSearchQuery}
                onSubmitEditing={searchAddress}
                returnKeyType="search"
                autoCorrect={false}
              />
              <TouchableOpacity style={s.searchBtn} onPress={searchAddress} disabled={searchBusy}>
                {searchBusy
                  ? <ActivityIndicator color="#000" size="small" />
                  : <Text style={s.searchBtnTxt}>🔍</Text>}
              </TouchableOpacity>
            </View>

            {searchResults.length > 0 && (
              <View style={s.searchList} pointerEvents="auto">
                {searchResults.map((r, i) => (
                  <TouchableOpacity
                    key={i}
                    style={[s.searchResult, i > 0 && { borderTopWidth: 1, borderTopColor: C.border }]}
                    onPress={() => applySearchResult(r)}
                  >
                    <Text style={s.searchResultTxt} numberOfLines={2}>{r.display_name}</Text>
                  </TouchableOpacity>
                ))}
                <TouchableOpacity
                  style={[s.searchResult, { borderTopWidth: 1, borderTopColor: C.border, alignItems: 'center' }]}
                  onPress={() => setSearchResults([])}
                >
                  <Text style={[s.searchResultTxt, { color: C.dim, fontSize: 11 }]}>Schließen</Text>
                </TouchableOpacity>
              </View>
            )}

            <View style={s.modeHintBox}>
              <Text style={s.modeHintTxt}>{modeHint}</Text>
            </View>
          </View>

          <TouchableOpacity style={s.locBtn} onPress={useMyLocation}>
            <Text style={s.locBtnTxt}>◎</Text>
          </TouchableOpacity>
        </View>
      </View>

      {/* Mode bar */}
      <ScrollView horizontal showsHorizontalScrollIndicator={false} style={s.modeBar}
                  contentContainerStyle={{ paddingHorizontal: 8, gap: 6 }}>
        <ModeBtn label={`${isCircuit ? 'S/F' : 'Start'} 1${sf1 ? ' ✓' : ''}`}
                 active={mode==='sf1'} onPress={() => setMode('sf1')} />
        <ModeBtn label={`${isCircuit ? 'S/F' : 'Start'} 2${sf2 ? ' ✓' : ''}`}
                 active={mode==='sf2'} onPress={() => setMode('sf2')} />
        {!isCircuit && (
          <>
            <ModeBtn label={`Finish 1${fin1 ? ' ✓' : ''}`}
                     active={mode==='fin1'} onPress={() => setMode('fin1')} />
            <ModeBtn label={`Finish 2${fin2 ? ' ✓' : ''}`}
                     active={mode==='fin2'} onPress={() => setMode('fin2')} />
          </>
        )}
        <ModeBtn label={`+ Sektor (${sectors.length}/${MAX_SECTORS})`}
                 active={mode==='sector'}
                 onPress={() => setMode('sector')}
                 disabled={sectors.length >= MAX_SECTORS} />
        <TouchableOpacity style={[s.modeBtn, { borderColor: C.danger }]} onPress={clearAll}>
          <Text style={[s.modeBtnTxt, { color: C.danger }]}>Reset</Text>
        </TouchableOpacity>
      </ScrollView>

      {/* Form below — scrollable */}
      <ScrollView style={{ flex: 1 }} contentContainerStyle={{ padding: 12, paddingBottom: 40 }}>
        <Text style={s.section}>Name</Text>
        <TextInput
          style={s.input}
          placeholder="z. B. Nürburgring GP"
          placeholderTextColor={C.dim}
          value={name}
          onChangeText={setName}
          maxLength={47}
        />

        <View style={s.row2}>
          <View style={{ flex: 2 }}>
            <Text style={s.section}>Land</Text>
            <TextInput style={s.input} placeholder="DE" placeholderTextColor={C.dim}
              value={country} onChangeText={setCountry} maxLength={31} />
          </View>
          <View style={{ flex: 3 }}>
            <Text style={s.section}>Länge (km, opt.)</Text>
            <TextInput style={s.input} placeholder="4.5" placeholderTextColor={C.dim}
              keyboardType="numbers-and-punctuation"
              value={lengthKm} onChangeText={setLengthKm} />
          </View>
        </View>

        <Text style={s.section}>Typ</Text>
        <View style={s.typeRow}>
          <TouchableOpacity style={[s.typeBtn, isCircuit && s.typeBtnOn]}
                            onPress={() => setIsCircuit(true)}>
            <Text style={[s.typeBtnTxt, isCircuit && s.typeBtnTxtOn]}>Rundkurs</Text>
          </TouchableOpacity>
          <TouchableOpacity style={[s.typeBtn, !isCircuit && s.typeBtnOn]}
                            onPress={() => setIsCircuit(false)}>
            <Text style={[s.typeBtnTxt, !isCircuit && s.typeBtnTxtOn]}>A-B Stage</Text>
          </TouchableOpacity>
        </View>

        {/* Point summary */}
        <Text style={s.section}>Punkte</Text>
        <PointRow label={isCircuit ? 'S/F Punkt 1' : 'Start Punkt 1'}
                  p={sf1} onEdit={() => setMode('sf1')} />
        <PointRow label={isCircuit ? 'S/F Punkt 2' : 'Start Punkt 2'}
                  p={sf2} onEdit={() => setMode('sf2')} />
        {!isCircuit && (
          <>
            <PointRow label="Finish Punkt 1" p={fin1} onEdit={() => setMode('fin1')} />
            <PointRow label="Finish Punkt 2" p={fin2} onEdit={() => setMode('fin2')} />
          </>
        )}
        {sectors.map((p, i) => (
          <View key={i} style={s.ptRow}>
            <Text style={s.ptLbl}>Sektor {i + 1}</Text>
            <Text style={s.ptVal}>{fmtCoord(p.lat)}, {fmtCoord(p.lon)}</Text>
            <TouchableOpacity onPress={() => delSector(i)} style={s.delBtn}>
              <Text style={s.delBtnTxt}>✕</Text>
            </TouchableOpacity>
          </View>
        ))}

        <TouchableOpacity
          style={[s.saveBtn, (!canSave || busy) && { opacity: 0.4 }]}
          onPress={saveToDevice} disabled={!canSave || busy}>
          {busy
            ? <ActivityIndicator color="#000" />
            : <Text style={s.saveBtnTxt}>Auf Gerät senden</Text>}
        </TouchableOpacity>

        <TouchableOpacity
          style={[s.saveBtnAlt, (!canSave || busy) && { opacity: 0.4 }]}
          onPress={saveLocalOnly} disabled={!canSave || busy}>
          <Text style={s.saveBtnAltTxt}>Nur lokal speichern</Text>
        </TouchableOpacity>

        <Text style={s.note}>
          "Auf Gerät senden" überträgt sofort, wenn der Laptimer verbunden ist —
          sonst landet die Strecke automatisch in der Upload-Warteschlange auf
          dem Handy. "Nur lokal speichern" hebt die Strecke immer auf dem Handy
          auf; du kannst sie später vom Home-Screen aus hochladen.
        </Text>
      </ScrollView>
    </KeyboardAvoidingView>
  );
}

function ModeBtn({ label, active, onPress, disabled }: {
  label: string; active: boolean; onPress: () => void; disabled?: boolean;
}) {
  return (
    <TouchableOpacity
      style={[s.modeBtn, active && s.modeBtnActive, disabled && { opacity: 0.4 }]}
      onPress={onPress} disabled={disabled}>
      <Text style={[s.modeBtnTxt, active && s.modeBtnTxtActive]}>{label}</Text>
    </TouchableOpacity>
  );
}

function PointRow({ label, p, onEdit }: {
  label: string; p: LatLon | null; onEdit: () => void;
}) {
  return (
    <TouchableOpacity style={s.ptRow} onPress={onEdit}>
      <Text style={s.ptLbl}>{label}</Text>
      <Text style={[s.ptVal, !p && { color: C.dim, fontStyle: 'italic' }]}>
        {p ? `${fmtCoord(p.lat)}, ${fmtCoord(p.lon)}` : 'nicht gesetzt — antippen'}
      </Text>
    </TouchableOpacity>
  );
}

// ── Leaflet HTML ─────────────────────────────────────────────────────────
const MAP_HTML = `<!DOCTYPE html>
<html><head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=4,user-scalable=yes"/>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<style>
  html,body{margin:0;padding:0;background:#0d1117;height:100%;overflow:hidden;}
  #map{width:100%;height:100vh;background:#0d1117;}
  .leaflet-container{background:#0d1117;}
  .leaflet-tile-pane.osm-tinted{filter:brightness(0.9) saturate(0.8);}
  .leaflet-control-zoom a{background:rgba(13,17,23,0.9);color:#fff;border:none;
    width:34px;height:34px;line-height:34px;font-size:18px;}
  .leaflet-control-zoom a:hover{background:#0096FF;color:#000;}
  .pt-label{background:rgba(0,0,0,0.8);color:#fff;border:1px solid #0096FF;
    border-radius:4px;padding:1px 5px;font:bold 10px sans-serif;white-space:nowrap;}
  #layerToggle{position:absolute;bottom:10px;left:10px;z-index:1000;
    display:flex;background:rgba(13,17,23,0.92);border:1px solid #1C3A5C;
    border-radius:22px;overflow:hidden;font-family:sans-serif;font-size:12px;
    box-shadow:0 2px 6px rgba(0,0,0,0.5);}
  #layerToggle button{background:transparent;color:#7A8FA6;border:none;
    padding:10px 16px;font-size:12px;font-weight:700;cursor:pointer;
    min-height:44px;}
  #layerToggle button.active{background:#0096FF;color:#000;}
</style>
</head><body>
<div id="map"></div>
<div id="layerToggle">
  <button id="btnMap" class="active">Karte</button>
  <button id="btnSat">Satellit</button>
</div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
const map = L.map('map',{zoomControl:true,attributionControl:false,tap:true})
  .setView([51.0, 10.0], 6);

// Two tile layers — user toggles via the buttons at the top-right.
// OSM: dark-tinted vector-style basemap. ESRI World Imagery: satellite.
const osmLayer = L.tileLayer(
  'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
  { maxZoom: 20, attribution: '© OpenStreetMap' });
const satLayer = L.tileLayer(
  'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
  { maxZoom: 19, attribution: 'Tiles © Esri' });

let activeLayer = 'map';
function setLayer(which) {
  const tilePane = map.getPane('tilePane');
  if (which === 'sat') {
    if (map.hasLayer(osmLayer)) map.removeLayer(osmLayer);
    if (!map.hasLayer(satLayer)) satLayer.addTo(map);
    tilePane.classList.remove('osm-tinted');
  } else {
    if (map.hasLayer(satLayer)) map.removeLayer(satLayer);
    if (!map.hasLayer(osmLayer)) osmLayer.addTo(map);
    tilePane.classList.add('osm-tinted');
  }
  activeLayer = which;
  document.getElementById('btnMap').classList.toggle('active', which === 'map');
  document.getElementById('btnSat').classList.toggle('active', which === 'sat');
}
setLayer('map');
document.getElementById('btnMap').addEventListener('click', () => setLayer('map'));
document.getElementById('btnSat').addEventListener('click', () => setLayer('sat'));

let currentMode = null;
let markers = {};        // keyed: sf1, sf2, fin1, fin2, sector_0 ...
let lineSF = null, lineFin = null;

function clearMarker(key){
  if(markers[key]){ map.removeLayer(markers[key]); delete markers[key]; }
}
function clearAllSectors(){
  Object.keys(markers).filter(k=>k.startsWith('sector_')).forEach(clearMarker);
}

function mkMarker(latlng, color, label, key){
  // Larger touch target (28 px), draggable so the user can drag any
  // existing point to a new position without first selecting a mode.
  const icon = L.divIcon({
    className:'',
    html:'<div style="width:24px;height:24px;border-radius:50%;'+
         'background:'+color+';border:3px solid #fff;box-shadow:0 0 8px #000;'+
         'transform:translate(-50%,-50%);"></div>',
    iconSize:[24,24], iconAnchor:[0,0],
  });
  const m = L.marker(latlng,{icon, draggable: true, autoPan: true}).addTo(map);
  if(label){
    m.bindTooltip(label,{permanent:true,direction:'top',offset:[0,-12],className:'pt-label'});
  }
  // While dragging redraw lines live so user sees the new geometry.
  m.on('drag', () => redrawLines());
  // On dragend send the new coords back to RN — host code updates the
  // sf1/sf2/fin1/fin2/sectors[] React state and the next markers-message
  // round-trips back here. No mode-button required.
  // Force-enable dragging in case Leaflet's auto-init missed it on touch.
  if (m.dragging) try { m.dragging.enable(); } catch (_) {}
  m.on('dragend', () => {
    const ll = m.getLatLng();
    const payload = JSON.stringify({
      type: 'drag', key: key, lat: ll.lat, lon: ll.lng,
    });
    // Belt + braces: try both message bridges
    if (window.ReactNativeWebView) {
      window.ReactNativeWebView.postMessage(payload);
    } else if (typeof postMessage === 'function') {
      postMessage(payload);
    }
  });
  return m;
}

function redrawLines(){
  if(lineSF){ map.removeLayer(lineSF); lineSF=null; }
  if(lineFin){ map.removeLayer(lineFin); lineFin=null; }
  if(markers.sf1 && markers.sf2){
    lineSF = L.polyline([markers.sf1.getLatLng(), markers.sf2.getLatLng()],
      {color:'#44E0A8',weight:4,opacity:0.9}).addTo(map);
  }
  if(markers.fin1 && markers.fin2){
    lineFin = L.polyline([markers.fin1.getLatLng(), markers.fin2.getLatLng()],
      {color:'#FF3B30',weight:4,opacity:0.9}).addTo(map);
  }
}

function setMarkers(msg){
  ['sf1','sf2','fin1','fin2'].forEach(k=>clearMarker(k));
  clearAllSectors();
  const COLOR = { sf1:'#44E0A8', sf2:'#44E0A8', fin1:'#FF3B30', fin2:'#FF3B30' };
  // Map-marker tooltips. S/F wird im A-B-Fall durch msg.showFin umetikettiert:
  // wir haben hier keine isCircuit-Info, senden zusätzlich msg.sfLabel aus RN.
  const LABEL = {
    sf1:  msg.sfLabel ? (msg.sfLabel + ' 1') : 'S/F 1',
    sf2:  msg.sfLabel ? (msg.sfLabel + ' 2') : 'S/F 2',
    fin1: 'Finish 1',
    fin2: 'Finish 2',
  };
  ['sf1','sf2'].forEach(k=>{
    const p = msg[k]; if(p) markers[k]=mkMarker([p.lat,p.lon],COLOR[k],LABEL[k],k);
  });
  if(msg.showFin){
    ['fin1','fin2'].forEach(k=>{
      const p = msg[k]; if(p) markers[k]=mkMarker([p.lat,p.lon],COLOR[k],LABEL[k],k);
    });
  }
  (msg.sectors||[]).forEach((p,i)=>{
    markers['sector_'+i] = mkMarker([p.lat,p.lon],'#FFEE55','S'+(i+1),'sector_'+i);
  });
  redrawLines();
}

map.on('click', e => {
  if(!currentMode) return;
  const payload = JSON.stringify({type:'tap', lat:e.latlng.lat, lon:e.latlng.lng});
  if(window.ReactNativeWebView) window.ReactNativeWebView.postMessage(payload);
});

window.__handle = function(raw){
  try{
    const msg = JSON.parse(raw);
    if(msg.type==='center'){
      map.setView([msg.lat,msg.lon], msg.zoom || 17);
    } else if(msg.type==='markers'){
      setMarkers(msg);
    } else if(msg.type==='mode'){
      currentMode = msg.mode;
    }
  }catch(e){}
};
</script>
</body></html>`;

const s = StyleSheet.create({
  mapWrap:    { height: 340, backgroundColor: C.surface, borderBottomWidth: 1, borderBottomColor: C.border },
  map:        { flex: 1, backgroundColor: C.surface },
  mapOverlay: { ...StyleSheet.absoluteFillObject, padding: 10, justifyContent: 'space-between' },

  searchBox:  { flexDirection: 'row', alignItems: 'center', gap: 6,
                backgroundColor: 'rgba(0,0,0,0.82)', borderRadius: 10,
                paddingHorizontal: 8, paddingVertical: 4,
                borderWidth: 1, borderColor: C.border, marginBottom: 6 },
  searchInput:{ flex: 1, color: C.text, fontSize: 13, paddingVertical: 8 },
  searchBtn:  { width: 36, height: 36, borderRadius: 8, backgroundColor: C.accent,
                alignItems: 'center', justifyContent: 'center' },
  searchBtnTxt:{ fontSize: 15 },
  searchList: { backgroundColor: 'rgba(0,0,0,0.92)', borderRadius: 10,
                borderWidth: 1, borderColor: C.border, marginBottom: 6,
                maxHeight: 240, overflow: 'hidden' },
  searchResult:{ paddingHorizontal: 10, paddingVertical: 8 },
  searchResultTxt:{ color: C.text, fontSize: 12 },

  modeHintBox:{ alignSelf: 'center', backgroundColor: 'rgba(0,0,0,0.78)',
                paddingHorizontal: 12, paddingVertical: 6, borderRadius: 14,
                borderWidth: 1, borderColor: C.accent },
  modeHintTxt:{ color: '#fff', fontSize: 12, fontWeight: '700' },
  locBtn:     { alignSelf: 'flex-end', width: 44, height: 44, borderRadius: 22,
                backgroundColor: 'rgba(0,150,255,0.95)', alignItems: 'center',
                justifyContent: 'center', borderWidth: 2, borderColor: '#fff' },
  locBtnTxt:  { color: '#fff', fontSize: 22, fontWeight: '900', marginTop: -2 },

  modeBar:    { flexGrow: 0, paddingVertical: 8, backgroundColor: C.surface,
                borderBottomWidth: 1, borderBottomColor: C.border },
  modeBtn:    { paddingHorizontal: 12, paddingVertical: 7, borderRadius: 16,
                borderWidth: 1, borderColor: C.border, backgroundColor: C.surface2 },
  modeBtnActive: { backgroundColor: C.accent, borderColor: C.accent },
  modeBtnTxt: { color: C.text, fontSize: 12, fontWeight: '600' },
  modeBtnTxtActive: { color: '#000', fontWeight: '800' },

  section:    { color: C.dim, fontSize: 11, fontWeight: '700',
                textTransform: 'uppercase', marginTop: 14, marginBottom: 6 },
  input:      { backgroundColor: C.surface, color: C.text, borderRadius: 8,
                paddingHorizontal: 12, paddingVertical: 10, fontSize: 15,
                borderWidth: 1, borderColor: C.border },
  row2:       { flexDirection: 'row', gap: 10 },

  typeRow:    { flexDirection: 'row', gap: 8 },
  typeBtn:    { flex: 1, backgroundColor: C.surface, borderRadius: 8,
                paddingVertical: 12, alignItems: 'center',
                borderWidth: 1, borderColor: C.border },
  typeBtnOn:  { backgroundColor: C.accent, borderColor: C.accent },
  typeBtnTxt: { color: C.text, fontWeight: '600' },
  typeBtnTxtOn:{ color: '#000', fontWeight: '800' },

  ptRow:      { flexDirection: 'row', alignItems: 'center', gap: 10,
                paddingVertical: 8, paddingHorizontal: 10, marginBottom: 6,
                backgroundColor: C.surface, borderRadius: 8,
                borderWidth: 1, borderColor: C.border },
  ptLbl:      { color: C.text, fontSize: 13, fontWeight: '600', width: 100 },
  ptVal:      { flex: 1, color: C.text, fontSize: 13, fontVariant: ['tabular-nums'] },
  delBtn:     { width: 30, height: 30, borderRadius: 15, backgroundColor: C.surface2,
                alignItems: 'center', justifyContent: 'center',
                borderWidth: 1, borderColor: C.border },
  delBtnTxt:  { color: C.danger ?? '#F44', fontSize: 13, fontWeight: '700' },

  saveBtn:    { backgroundColor: C.accent, borderRadius: 10,
                paddingVertical: 14, alignItems: 'center', marginTop: 20 },
  saveBtnTxt: { color: '#000', fontSize: 15, fontWeight: '800' },
  saveBtnAlt: { backgroundColor: C.surface2, borderRadius: 10,
                paddingVertical: 12, alignItems: 'center', marginTop: 10,
                borderWidth: 1, borderColor: C.border },
  saveBtnAltTxt: { color: C.text, fontSize: 14, fontWeight: '700' },
  note:       { color: C.dim, fontSize: 11, marginTop: 14,
                textAlign: 'center', paddingHorizontal: 20 },
});
