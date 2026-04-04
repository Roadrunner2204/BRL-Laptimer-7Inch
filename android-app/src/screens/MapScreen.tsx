import React, { useEffect, useRef, useState } from 'react';
import {
  View, Text, StyleSheet, TouchableOpacity, ScrollView, Switch,
} from 'react-native';
import { WebView } from 'react-native-webview';
import { RouteProp } from '@react-navigation/native';
import { RootStackParamList } from '../App';
import { loadSession } from '../storage';
import { Session } from '../types';
import { fmtTime } from '../utils';
import { MAP_HTML } from '../mapTemplate';
import { C } from '../theme';

type Props = { route: RouteProp<RootStackParamList, 'Map'> };

export default function MapScreen({ route }: Props) {
  const webviewRef = useRef<WebView>(null);
  const [session, setSession]       = useState<Session | null>(null);
  const [ready, setReady]           = useState(false);
  const [lapVis, setLapVis]         = useState<boolean[]>([]);
  const [speedColor, setSpeedColor] = useState(true);
  const [sheetOpen, setSheetOpen]   = useState(true);

  useEffect(() => {
    loadSession(route.params.sessionId).then(s => {
      if (s) {
        setSession(s);
        setLapVis(s.laps.map(() => true));
      }
    });
  }, [route.params.sessionId]);

  function postMsg(msg: object) {
    webviewRef.current?.injectJavaScript(
      `handleMsg(${JSON.stringify(JSON.stringify(msg))}); true;`
    );
  }

  function onWebViewReady() {
    setReady(true);
    if (session) {
      postMsg({ type: 'session', data: session });
    }
  }

  useEffect(() => {
    if (ready && session) postMsg({ type: 'session', data: session });
  }, [ready, session]);

  function toggleLap(idx: number) {
    const next = [...lapVis];
    next[idx] = !next[idx];
    setLapVis(next);
    postMsg({ type: 'toggleLap', lapIdx: idx, visible: next[idx] });
  }

  function onSpeedColorChange(v: boolean) {
    setSpeedColor(v);
    postMsg({ type: 'speedColor', enabled: v });
  }

  const LAP_COLORS = ['#00CC66','#4488FF','#FF8800','#FF44AA','#44CCFF','#FFFF44','#FF4444','#AA44FF'];

  return (
    <View style={s.root}>
      <WebView
        ref={webviewRef}
        source={{ html: MAP_HTML }}
        style={s.map}
        onLoadEnd={onWebViewReady}
        javaScriptEnabled
        domStorageEnabled
        originWhitelist={['*']}
        mixedContentMode="always"
      />

      {/* Bottom sheet toggle */}
      <TouchableOpacity style={s.sheetToggle} onPress={() => setSheetOpen(v => !v)}>
        <Text style={s.sheetToggleTxt}>{sheetOpen ? '▼ Runden' : '▲ Runden'}</Text>
      </TouchableOpacity>

      {sheetOpen && session && (
        <View style={s.sheet}>
          {/* Speed color toggle */}
          <View style={s.speedRow}>
            <Text style={s.speedLbl}>Geschwindigkeits-Farbgebung (Beste Runde)</Text>
            <Switch
              value={speedColor}
              onValueChange={onSpeedColorChange}
              trackColor={{ true: C.accent, false: '#333' }}
              thumbColor="#fff"
            />
          </View>

          {/* Lap toggles */}
          <ScrollView horizontal showsHorizontalScrollIndicator={false} style={s.lapScroll}>
            {session.laps.map((lap, i) => {
              const isBest = i === session.best_lap_idx;
              const color = isBest ? C.accent : LAP_COLORS[i % LAP_COLORS.length];
              const on = lapVis[i] ?? true;
              return (
                <TouchableOpacity
                  key={i}
                  style={[s.lapChip, { borderColor: color }, !on && s.lapChipOff]}
                  onPress={() => toggleLap(i)}
                >
                  <View style={[s.chipDot, { backgroundColor: on ? color : '#333' }]} />
                  <Text style={[s.chipTxt, !on && { color: C.dim }]}>
                    R{lap.lap}{isBest ? ' ★' : ''}
                  </Text>
                  <Text style={[s.chipTime, !on && { color: '#444' }]}>{fmtTime(lap.total_ms)}</Text>
                </TouchableOpacity>
              );
            })}
          </ScrollView>
        </View>
      )}
    </View>
  );
}

const s = StyleSheet.create({
  root:          { flex:1, backgroundColor: C.bg },
  map:           { flex:1 },
  sheetToggle:   { position:'absolute', top:12, right:12, backgroundColor:'rgba(0,0,0,0.75)',
                   borderRadius:20, paddingHorizontal:14, paddingVertical:6 },
  sheetToggleTxt:{ color: C.text, fontSize:13, fontWeight:'600' },
  sheet:         { backgroundColor:'rgba(13,13,13,0.95)', borderTopWidth:1, borderColor:'#222',
                   paddingTop:8, paddingBottom:12 },
  speedRow:      { flexDirection:'row', alignItems:'center', justifyContent:'space-between',
                   paddingHorizontal:16, paddingBottom:8 },
  speedLbl:      { color: C.dim, fontSize:12, flex:1, marginRight:8 },
  lapScroll:     { paddingHorizontal:12 },
  lapChip:       { backgroundColor: C.surface, borderRadius:8, borderWidth:1.5, padding:8,
                   marginRight:8, alignItems:'center', minWidth:90 },
  lapChipOff:    { opacity:0.5 },
  chipDot:       { width:10, height:4, borderRadius:2, marginBottom:4 },
  chipTxt:       { color: C.text, fontWeight:'700', fontSize:13 },
  chipTime:      { color: C.dim, fontSize:11, marginTop:2 },
});
