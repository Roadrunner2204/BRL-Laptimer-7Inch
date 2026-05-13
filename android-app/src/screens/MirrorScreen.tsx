/**
 * MirrorScreen -- live screen-mirror of the laptimer display.
 *
 * MJPEG stream from the firmware (port 8081) is displayed inside a
 * pointerEvents-disabled WebView (RN's <Image> doesn't grok MJPEG, but
 * a stock <img> tag inside a WebView handles multipart/x-mixed-replace
 * out of the box). A transparent touch overlay sits on top to capture
 * gestures and forward them to the laptimer as logical-pixel taps.
 *
 * Coordinate mapping: the image is drawn with object-fit:contain so it
 * stays at 1024:600 even when the phone's aspect ratio is different.
 * We measure the overlay onLayout, derive the contained-image rect, and
 * map each tap pixel to (0..1023, 0..599) before POSTing.
 *
 * Touch throttling: every press/release is forwarded immediately so
 * LVGL gets clean click semantics. Move events are throttled to 50 ms
 * (~20 Hz) so a fast drag doesn't flood the AP with hundreds of POSTs.
 */

import React, { useEffect, useMemo, useRef, useState } from 'react';
import {
  View, Text, StyleSheet, ActivityIndicator,
  PanResponder, GestureResponderEvent, LayoutChangeEvent,
  TouchableOpacity, Alert, TextInput, Platform,
} from 'react-native';
import { WebView } from 'react-native-webview';
import { NativeStackNavigationProp } from '@react-navigation/native-stack';
import { RootStackParamList } from '../App';
import { C } from '../theme';
import {
  mirrorMjpegUrl, postTouch, postTextEdit, MIRROR_W, MIRROR_H,
} from '../api';

type Props = {
  navigation: NativeStackNavigationProp<RootStackParamList, 'Mirror'>;
};

const MIRROR_RATIO = MIRROR_W / MIRROR_H;
const MOVE_THROTTLE_MS = 50;

interface ImageRect { x: number; y: number; w: number; h: number; }

function computeImageRect(viewW: number, viewH: number): ImageRect {
  // object-fit: contain — image keeps 1024:600 aspect, letterboxed.
  if (viewW <= 0 || viewH <= 0) return { x: 0, y: 0, w: 0, h: 0 };
  const viewRatio = viewW / viewH;
  if (viewRatio > MIRROR_RATIO) {
    const h = viewH;
    const w = h * MIRROR_RATIO;
    return { x: (viewW - w) / 2, y: 0, w, h };
  } else {
    const w = viewW;
    const h = w / MIRROR_RATIO;
    return { x: 0, y: (viewH - h) / 2, w, h };
  }
}

export default function MirrorScreen({ navigation }: Props) {
  const [layout, setLayout] = useState({ w: 0, h: 0 });
  const [streamErr, setStreamErr] = useState<string | null>(null);
  const [postErr, setPostErr] = useState<string | null>(null);
  const [pressed, setPressed] = useState(false);
  const [tapCount, setTapCount] = useState(0);
  const [lastTap, setLastTap] = useState<string>('--');
  const [kbdOpen, setKbdOpen] = useState(false);
  const [kbdBuf, setKbdBuf] = useState('');
  const [kbdHint, setKbdHint] = useState<string | null>(null);
  const lastMoveRef = useRef(0);
  const lastSentTextRef = useRef('');
  const inputRef = useRef<TextInput>(null);
  // Track the last sent point so we can re-issue it on release if the
  // user lifted finger during a throttled-out move event.
  const lastPointRef = useRef<{ x: number; y: number } | null>(null);

  const imgUrl = useMemo(() => mirrorMjpegUrl(), []);

  const html = useMemo(() => `
    <!doctype html>
    <html>
    <head>
      <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
      <style>
        html, body {
          margin: 0; padding: 0; background: #000;
          width: 100%; height: 100%;
          overflow: hidden;
          touch-action: none;
          -webkit-user-select: none;
          user-select: none;
        }
        img {
          display: block;
          width: 100%; height: 100%;
          object-fit: contain;
          pointer-events: none;
          -webkit-user-drag: none;
          -webkit-touch-callout: none;
        }
      </style>
    </head>
    <body>
      <img src="${imgUrl}" />
    </body>
    </html>
  `, [imgUrl]);

  function onOverlayLayout(e: LayoutChangeEvent) {
    const { width, height } = e.nativeEvent.layout;
    setLayout({ w: width, h: height });
  }

  function mapToDisplay(touchX: number, touchY: number): { x: number; y: number } | null {
    const rect = computeImageRect(layout.w, layout.h);
    if (rect.w <= 0 || rect.h <= 0) return null;
    const lx = touchX - rect.x;
    const ly = touchY - rect.y;
    // Outside the contained-image area -- ignore so taps in the letterbox
    // don't get clamped to an edge of the laptimer UI.
    if (lx < 0 || ly < 0 || lx > rect.w || ly > rect.h) return null;
    return {
      x: (lx / rect.w) * MIRROR_W,
      y: (ly / rect.h) * MIRROR_H,
    };
  }

  async function send(x: number, y: number, down: boolean) {
    const ix = Math.round(x);
    const iy = Math.round(y);
    setTapCount(c => c + 1);
    setLastTap(`${ix},${iy} ${down ? 'D' : 'U'}`);
    try {
      await postTouch(x, y, down);
      if (postErr) setPostErr(null);
    } catch (e: any) {
      const msg = e?.message ?? String(e);
      console.warn('[mirror] postTouch failed:', msg);
      setPostErr(msg);
    }
  }

  // Pan responder accepts taps + drags. We fire press, move (throttled),
  // and release events through to the laptimer. Pinch / multi-touch is
  // intentionally NOT supported -- the GT911 on the panel doesn't expose
  // multi-touch in the LVGL pointer indev anyway.
  const isPressedRef = useRef(false);
  const panResponder = useMemo(() => {
    const releaseOnce = () => {
      if (!isPressedRef.current) return;
      isPressedRef.current = false;
      const p = lastPointRef.current;
      setPressed(false);
      if (p) send(p.x, p.y, false);
    };
    return PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponder:  () => true,
      onPanResponderTerminationRequest: () => false,
      onPanResponderGrant: (e: GestureResponderEvent) => {
        const p = mapToDisplay(e.nativeEvent.locationX, e.nativeEvent.locationY);
        if (!p) return;
        lastPointRef.current = p;
        isPressedRef.current = true;
        setPressed(true);
        send(p.x, p.y, true);
      },
      onPanResponderMove: (e: GestureResponderEvent) => {
        const now = Date.now();
        if (now - lastMoveRef.current < MOVE_THROTTLE_MS) return;
        const p = mapToDisplay(e.nativeEvent.locationX, e.nativeEvent.locationY);
        if (!p) return;
        lastMoveRef.current = now;
        lastPointRef.current = p;
        send(p.x, p.y, true);
      },
      // Release + Terminate can both fire for the same gesture (RN's
      // responder system hands the gesture off once if a parent claims
      // it). Dedupe via isPressedRef so LVGL doesn't get a phantom
      // second release that briefly clobbers a subsequent press.
      onPanResponderRelease:   releaseOnce,
      onPanResponderTerminate: releaseOnce,
    });
  }, [layout.w, layout.h]);

  // Friendly hint if no firmware is connected -- otherwise the WebView just
  // shows a black square forever and the user wonders why.
  useEffect(() => {
    if (!imgUrl) {
      Alert.alert('Nicht verbunden',
        'Bitte zuerst auf dem Home-Screen mit dem Laptimer verbinden.',
        [{ text: 'OK', onPress: () => navigation.goBack() }]);
    }
  }, [imgUrl, navigation]);

  // Phone-keyboard relay. Each onChangeText fires a diff vs the last sent
  // text -- characters added go via {add:...}, characters removed via
  // {bs:N}. Network races are safe because /text is monotonic on the
  // textarea: appends order-independent for single chars; bs always lands
  // before the subsequent append within one POST.
  function openKeyboard() {
    setKbdBuf('');
    lastSentTextRef.current = '';
    setKbdOpen(true);
    setKbdHint(null);
    // Focus on next tick so the TextInput is mounted first.
    setTimeout(() => inputRef.current?.focus(), 50);
  }
  function closeKeyboard() {
    inputRef.current?.blur();
    setKbdOpen(false);
  }

  async function onKbdChange(newText: string) {
    const oldText = lastSentTextRef.current;
    if (newText === oldText) return;

    // Find longest common prefix to compute minimal {bs, add} pair.
    let i = 0;
    const minLen = Math.min(oldText.length, newText.length);
    while (i < minLen && oldText.charCodeAt(i) === newText.charCodeAt(i)) i++;
    const bs  = oldText.length - i;
    const add = newText.slice(i);

    setKbdBuf(newText);
    lastSentTextRef.current = newText;

    try {
      const args: { add?: string; bs?: number } = {};
      if (bs  > 0) args.bs  = bs;
      if (add)     args.add = add;
      if (!args.add && !args.bs) return;
      const r = await postTextEdit(args);
      if (!r.hit) {
        setKbdHint('Erst auf ein Textfeld am Display tippen, damit die Tastatur erscheint.');
      } else if (kbdHint) {
        setKbdHint(null);
      }
    } catch (e: any) {
      setPostErr(e?.message ?? String(e));
    }
  }

  return (
    <View style={s.root}>
      <View style={s.canvas}>
        <WebView
          style={s.web}
          containerStyle={s.web}
          originWhitelist={['*']}
          source={{ html }}
          // We capture all touches in the overlay; the WebView just renders
          // the image. Disabling its native scrolling + bounce removes the
          // tiny vertical jitter that otherwise happens on quick taps.
          scrollEnabled={false}
          showsHorizontalScrollIndicator={false}
          showsVerticalScrollIndicator={false}
          bounces={false}
          overScrollMode="never"
          androidLayerType="hardware"
          mixedContentMode="always"
          onError={({ nativeEvent }) =>
            setStreamErr(nativeEvent?.description ?? 'WebView error')}
          onHttpError={({ nativeEvent }) =>
            setStreamErr(`HTTP ${nativeEvent?.statusCode}`)}
        />
        {/* Transparent touch capture. pointerEvents on the WebView itself
            is unreliable on Android -- a sibling overlay is the safe bet. */}
        <View
          style={s.overlay}
          onLayout={onOverlayLayout}
          {...panResponder.panHandlers}
        />
        {pressed && <View style={s.pressIndicator} pointerEvents="none" />}
      </View>

      <View style={s.hud} pointerEvents="box-none">
        <TouchableOpacity style={s.backBtn} onPress={() => navigation.goBack()}>
          <Text style={s.backTxt}>‹  Zurück</Text>
        </TouchableOpacity>
        <View style={s.hudRight}>
          <TouchableOpacity
            style={[s.kbdBtn, kbdOpen && s.kbdBtnOn]}
            onPress={() => (kbdOpen ? closeKeyboard() : openKeyboard())}
          >
            <Text style={s.kbdBtnTxt}>⌨</Text>
          </TouchableOpacity>
          <View style={s.statusPill}>
            <ActivityIndicator color={C.accent} size="small" />
            <Text style={s.statusTxt}>
              {streamErr ? `Stream: ${streamErr}` : `Live · ${tapCount}: ${lastTap}`}
            </Text>
          </View>
        </View>
      </View>

      {/* Hidden TextInput captures the system keyboard. Positioned offscreen
          via opacity:0 + height:0 so it doesn't visually interfere; the
          system keyboard pops up from the bottom regardless. */}
      {kbdOpen && (
        <TextInput
          ref={inputRef}
          style={s.hiddenInput}
          value={kbdBuf}
          onChangeText={onKbdChange}
          autoCorrect={false}
          autoCapitalize="none"
          autoComplete="off"
          spellCheck={false}
          keyboardType="default"
          returnKeyType="done"
          blurOnSubmit={true}
          // Submit dismisses the keyboard. Do NOT append a literal '\n'
          // here -- the firmware would inject the newline into the active
          // textarea, which then ended up raw-encoded in the session JSON
          // and broke /sessions parsing for every spec-compliant client.
          onSubmitEditing={closeKeyboard}
          onBlur={() => setKbdOpen(false)}
        />
      )}

      {kbdOpen && kbdHint && (
        <View style={s.hintBanner} pointerEvents="none">
          <Text style={s.hintTxt}>{kbdHint}</Text>
        </View>
      )}

      {postErr && (
        <View style={s.errBanner} pointerEvents="none">
          <Text style={s.errTxt}>Touch-Fehler: {postErr}</Text>
        </View>
      )}
    </View>
  );
}

const s = StyleSheet.create({
  root:        { flex: 1, backgroundColor: '#000' },
  canvas:      { flex: 1, backgroundColor: '#000' },
  web:         { flex: 1, backgroundColor: '#000' },
  overlay:     { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0 },
  pressIndicator: {
    position: 'absolute', left: 0, right: 0, top: 0, bottom: 0,
    borderWidth: 1, borderColor: C.accent, opacity: 0.4,
  },
  hud: {
    position: 'absolute', top: 36, left: 12, right: 12,
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
  },
  backBtn: {
    backgroundColor: 'rgba(0,0,0,0.55)',
    paddingHorizontal: 12, paddingVertical: 8,
    borderRadius: 8,
  },
  backTxt: { color: C.text, fontSize: 14, fontWeight: '600' },
  hudRight: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  kbdBtn: {
    backgroundColor: 'rgba(0,0,0,0.55)',
    width: 40, height: 40, borderRadius: 8,
    alignItems: 'center', justifyContent: 'center',
  },
  kbdBtnOn:  { backgroundColor: C.accent },
  kbdBtnTxt: { color: C.text, fontSize: 20 },
  statusPill: {
    flexDirection: 'row', alignItems: 'center',
    backgroundColor: 'rgba(0,0,0,0.55)',
    paddingHorizontal: 10, paddingVertical: 6,
    borderRadius: 8,
  },
  statusTxt: { color: C.text, marginLeft: 8, fontSize: 12 },
  hiddenInput: {
    position: 'absolute', left: 0, top: 0,
    width: 1, height: 1, opacity: 0,
    color: 'transparent',
  },
  hintBanner: {
    position: 'absolute', left: 12, right: 12, bottom: 24,
    backgroundColor: 'rgba(0,150,255,0.85)',
    padding: 10, borderRadius: 8,
  },
  hintTxt: { color: '#fff', fontSize: 12 },
  errBanner: {
    position: 'absolute', left: 12, right: 12, bottom: 24,
    backgroundColor: 'rgba(255,59,48,0.85)',
    padding: 10, borderRadius: 8,
  },
  errTxt: { color: '#fff', fontSize: 12 },
});
