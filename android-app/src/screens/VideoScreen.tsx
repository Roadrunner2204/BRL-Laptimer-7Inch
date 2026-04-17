/**
 * VideoScreen — play a recorded AVI either directly from the device (stream
 * over WiFi) or from a cached local copy (after download).
 *
 * Phase 4 scope: basic playback + scrubber. Data overlay comes in Phase 5 —
 * we design the layout so a <Canvas> / SVG can sit on top of the <Video>
 * later without restructuring.
 */

import React, { useEffect, useMemo, useRef, useState } from 'react';
import {
  View, Text, StyleSheet, ActivityIndicator, TouchableOpacity, Alert,
  useWindowDimensions, ScrollView,
} from 'react-native';
import { Video, ResizeMode, AVPlaybackStatus } from 'expo-av';
import * as FileSystem from 'expo-file-system';
import * as Sharing from 'expo-sharing';
import { RouteProp } from '@react-navigation/native';
import { RootStackParamList } from '../App';
import { C } from '../theme';
import { videoUrl, deleteVideoOnDevice } from '../api';
import { loadSession } from '../storage';
import { Session } from '../types';
import { deriveChannels, deltaToReference, LapChannels } from '../analysis';
import VideoOverlay from '../components/VideoOverlay';
import { loadOverlayConfig, OverlayConfig, DEFAULT_OVERLAY_CONFIG } from '../overlayConfig';
import { useFocusEffect } from '@react-navigation/native';
import { NativeStackNavigationProp } from '@react-navigation/native-stack';

type Props = {
  route: RouteProp<RootStackParamList, 'Video'>;
  navigation: NativeStackNavigationProp<RootStackParamList, 'Video'>;
};

function fmtMs(ms: number): string {
  const total_s = Math.max(0, Math.floor(ms / 1000));
  const m = Math.floor(total_s / 60);
  const s = total_s % 60;
  return `${m}:${String(s).padStart(2, '0')}`;
}

export default function VideoScreen({ route, navigation }: Props) {
  const { width: winW } = useWindowDimensions();
  const videoRef = useRef<Video>(null);
  const [source, setSource] = useState<{ uri: string } | null>(null);
  const [status, setStatus] = useState<AVPlaybackStatus | null>(null);
  const [downloading, setDownloading] = useState(false);
  const [progressPct, setProgressPct] = useState<number | null>(null);   // 0..1, null = size unknown
  const [progressBytes, setProgressBytes] = useState<number>(0);
  const [totalBytes, setTotalBytes] = useState<number>(0);
  const dlResumable = useRef<FileSystem.DownloadResumable | null>(null);
  const [isLocal, setIsLocal] = useState(false);
  const [localPath, setLocalPath] = useState<string | null>(null);
  const [session, setSession] = useState<Session | null>(null);
  const [overlayLap, setOverlayLap] = useState<number | null>(null);
  const [showOverlay, setShowOverlay] = useState(true);
  const [overlayCfg, setOverlayCfg] = useState<OverlayConfig>(DEFAULT_OVERLAY_CONFIG);

  // Reload overlay config whenever this screen gains focus — so changes
  // made in OverlayConfigScreen take effect immediately.
  useFocusEffect(
    React.useCallback(() => {
      loadOverlayConfig().then(setOverlayCfg);
    }, [])
  );

  const { videoId, sessionId, mode } = route.params;
  // Session to load for overlay/channels may differ from the videoId — since
  // the firmware's per-lap split, videoId is a file stem (e.g. "<sid>_lap2")
  // while the session JSON is keyed by "<sid>". Fall back to videoId when
  // the caller didn't pass sessionId (legacy path from SessionsScreen).
  const sessionKey = sessionId ?? videoId;

  // Video aspect: assume 16:9 for now (player adapts; overlay fits to actual)
  const videoH = Math.round(winW * 9 / 16);

  // Load the matching session for overlay data.
  useEffect(() => {
    loadSession(sessionKey).then(s => {
      if (s) {
        setSession(s);
        // Prefer the lap whose video is actually playing, so the overlay
        // channels match the picture. Match by lap.video === videoId; fall
        // back to best_lap_idx when no match (legacy single-file videos).
        const lapIdx = s.laps.findIndex(l => l.video === videoId);
        setOverlayLap(lapIdx >= 0 ? lapIdx : s.best_lap_idx);
      }
    });
  }, [sessionKey, videoId]);

  const overlayChannels: LapChannels | null = useMemo(() => {
    if (!session || overlayLap == null) return null;
    const lap = session.laps[overlayLap];
    if (!lap) return null;
    return deriveChannels(lap);
  }, [session, overlayLap]);

  const overlayDelta: number[] | null = useMemo(() => {
    if (!session || overlayLap == null || !overlayChannels) return null;
    if (overlayLap === session.best_lap_idx) return null;
    const best = session.laps[session.best_lap_idx];
    if (!best) return null;
    const bestCh = deriveChannels(best);
    return deltaToReference(overlayChannels, bestCh);
  }, [session, overlayLap, overlayChannels]);

  useEffect(() => {
    init();
  }, [videoId, mode]);

  // Cancel any in-flight download when the screen unmounts so a leftover
  // native task doesn't keep writing to the cache dir after navigation.
  useEffect(() => {
    return () => {
      if (dlResumable.current) {
        dlResumable.current.cancelAsync().catch(() => {});
      }
    };
  }, []);

  async function init() {
    const localUri = FileSystem.documentDirectory + `videos/${videoId}.avi`;
    const info = await FileSystem.getInfoAsync(localUri);
    if (info.exists) {
      setSource({ uri: localUri });
      setIsLocal(true);
      setLocalPath(localUri);
    } else if (mode === 'stream') {
      setSource({ uri: videoUrl(videoId) });
      setIsLocal(false);
    } else {
      // Download-first mode — trigger download
      await doDownload(localUri);
    }
  }

  async function doDownload(targetUri?: string) {
    const target = targetUri ?? (FileSystem.documentDirectory + `videos/${videoId}.avi`);
    const dir = target.substring(0, target.lastIndexOf('/'));
    try {
      setDownloading(true);
      setProgressPct(null);
      setProgressBytes(0);
      setTotalBytes(0);
      await FileSystem.makeDirectoryAsync(dir, { intermediates: true }).catch(() => {});

      // createDownloadResumable streams chunks with a progress callback, so
      // the user sees a live byte count even on slow WiFi. Plain
      // downloadAsync has no progress hook and times out silently on large
      // AVI files. The server-side send_wait_timeout was raised to 30 s too.
      dlResumable.current = FileSystem.createDownloadResumable(
        videoUrl(videoId),
        target,
        {},
        ({ totalBytesWritten, totalBytesExpectedToWrite }) => {
          setProgressBytes(totalBytesWritten);
          if (totalBytesExpectedToWrite > 0) {
            setTotalBytes(totalBytesExpectedToWrite);
            setProgressPct(totalBytesWritten / totalBytesExpectedToWrite);
          }
        },
      );

      const res = await dlResumable.current.downloadAsync();
      if (!res) return;   // cancelled → dlResumable.cancelAsync resolved first
      if (res.status !== 200) throw new Error(`HTTP ${res.status}`);
      setSource({ uri: res.uri });
      setIsLocal(true);
      setLocalPath(res.uri);
    } catch (e: any) {
      const msg = e?.message ?? String(e);
      if (!/cancell?ed/i.test(msg)) {
        Alert.alert('Download fehlgeschlagen', msg);
      }
      setSource({ uri: videoUrl(videoId) });  // fall back to streaming
    } finally {
      dlResumable.current = null;
      setDownloading(false);
      setProgressPct(null);
      setProgressBytes(0);
      setTotalBytes(0);
    }
  }

  async function cancelDownload() {
    const r = dlResumable.current;
    if (!r) return;
    try {
      // cancelAsync deletes the partial file and rejects the pending
      // downloadAsync() promise. The catch in doDownload absorbs it.
      await r.cancelAsync();
    } catch { /* ignore — we're tearing down anyway */ }
  }

  async function doDeleteLocal() {
    if (!localPath) return;
    await FileSystem.deleteAsync(localPath, { idempotent: true });
    setLocalPath(null);
    setIsLocal(false);
    setSource({ uri: videoUrl(videoId) });
  }

  // Export the already-downloaded AVI via the native share sheet. User
  // picks the target (Downloads, Drive, WhatsApp, Email, …). We don't
  // touch the source file — it stays in the app's doc dir for continued
  // in-app playback. The AVI MIME is `video/avi`; some gallery apps
  // prefer `video/x-msvideo`, but the share sheet handles both.
  async function doExport() {
    if (!localPath) {
      Alert.alert('Kein lokales Video',
        'Bitte erst „Download" drücken, damit eine lokale Kopie entsteht.');
      return;
    }
    try {
      const available = await Sharing.isAvailableAsync();
      if (!available) {
        Alert.alert('Teilen nicht verfügbar',
          'Auf diesem Gerät steht kein Share-Dienst bereit.');
        return;
      }
      await Sharing.shareAsync(localPath, {
        mimeType: 'video/avi',
        dialogTitle: 'Video exportieren',
        UTI: 'public.avi',          // iOS hint, ignored on Android
      });
    } catch (e: any) {
      // User cancelling the share sheet resolves fine on Android; this
      // catch is for actual failures (permission, missing provider).
      Alert.alert('Export fehlgeschlagen', e?.message ?? String(e));
    }
  }

  async function doDeleteOnDevice() {
    Alert.alert('Video am Gerät löschen?', 'Das Video wird unwiderruflich von der HDD gelöscht.', [
      { text: 'Abbrechen', style: 'cancel' },
      { text: 'Löschen', style: 'destructive', onPress: async () => {
        try {
          await deleteVideoOnDevice(videoId);
          Alert.alert('Gelöscht', 'Video wurde vom Gerät entfernt.');
        } catch (e: any) {
          Alert.alert('Fehler', e?.message ?? String(e));
        }
      }},
    ]);
  }

  const posMs = status?.isLoaded ? status.positionMillis : 0;
  const durMs = status?.isLoaded ? (status.durationMillis ?? 0) : 0;
  const isPlaying = status?.isLoaded ? status.isPlaying : false;

  return (
    <View style={s.root}>
      {/* Video surface (16:9 aspect) */}
      <View style={[s.videoWrap, { height: videoH }]}>
        {source ? (
          <Video
            ref={videoRef}
            source={source}
            style={s.video}
            useNativeControls={false}
            resizeMode={ResizeMode.CONTAIN}
            onPlaybackStatusUpdate={setStatus}
            shouldPlay={false}
          />
        ) : (
          <ActivityIndicator color={C.accent} size="large" style={{ marginTop: 60 }} />
        )}

        {/* Telemetry HUD overlay — fully user-configurable */}
        {showOverlay && overlayChannels && status?.isLoaded && (
          <VideoOverlay
            width={winW}
            height={videoH}
            timeMs={status.positionMillis}
            channels={overlayChannels}
            delta={overlayDelta ?? undefined}
            lapNumber={session && overlayLap != null ? session.laps[overlayLap]?.lap : undefined}
            trackName={session?.track}
            config={overlayCfg}
          />
        )}

        {downloading && (
          <View style={s.overlayDownload}>
            <Text style={s.dlTxt}>Lade Video…</Text>
            {/* Deterministic bar when total size is known, otherwise
                indeterminate spinner + byte counter. */}
            {progressPct != null ? (
              <>
                <View style={s.dlBarTrack}>
                  <View style={[s.dlBarFill, { width: `${Math.round(progressPct * 100)}%` }]} />
                </View>
                <Text style={s.dlPctTxt}>
                  {Math.round(progressPct * 100)}%
                  {'  ·  '}
                  {(progressBytes / (1024 * 1024)).toFixed(1)} / {(totalBytes / (1024 * 1024)).toFixed(1)} MB
                </Text>
              </>
            ) : (
              <>
                <ActivityIndicator color={C.accent} size="large" style={{ marginVertical: 8 }} />
                {progressBytes > 0 && (
                  <Text style={s.dlPctTxt}>
                    {(progressBytes / (1024 * 1024)).toFixed(1)} MB geladen
                  </Text>
                )}
              </>
            )}
            <TouchableOpacity style={s.dlCancelBtn} onPress={cancelDownload}>
              <Text style={s.dlCancelTxt}>Abbrechen</Text>
            </TouchableOpacity>
          </View>
        )}
      </View>

      {/* Lap picker for overlay source */}
      {session && session.laps.length > 0 && (
        <ScrollView
          horizontal
          showsHorizontalScrollIndicator={false}
          style={s.lapBar}
          contentContainerStyle={{ paddingHorizontal: 10 }}
        >
          <TouchableOpacity
            style={[s.overlayChip, !showOverlay && s.overlayChipActive]}
            onPress={() => setShowOverlay(v => !v)}
          >
            <Text style={[s.overlayChipTxt, !showOverlay && { color: '#000' }]}>
              {showOverlay ? '◉ Overlay' : '○ Overlay'}
            </Text>
          </TouchableOpacity>
          <TouchableOpacity
            style={s.overlayChip}
            onPress={() => navigation.navigate('OverlayConfig')}
          >
            <Text style={s.overlayChipTxt}>⚙ Anpassen</Text>
          </TouchableOpacity>
          {session.laps.map((lap, i) => (
            <TouchableOpacity
              key={i}
              style={[s.lapChip, overlayLap === i && s.lapChipActive]}
              onPress={() => setOverlayLap(i)}
            >
              <Text style={[s.lapChipTxt, overlayLap === i && { color: '#000' }]}>
                R{lap.lap}{i === session.best_lap_idx ? '★' : ''}
              </Text>
            </TouchableOpacity>
          ))}
        </ScrollView>
      )}

      {/* Control bar */}
      <View style={s.controls}>
        <TouchableOpacity
          style={s.playBtn}
          onPress={() => {
            if (isPlaying) videoRef.current?.pauseAsync();
            else           videoRef.current?.playAsync();
          }}
          disabled={!status?.isLoaded}
        >
          <Text style={s.playTxt}>{isPlaying ? '❚❚' : '▶'}</Text>
        </TouchableOpacity>

        <Text style={s.time}>{fmtMs(posMs)} / {fmtMs(durMs)}</Text>

        <View style={{ flex: 1 }} />

        {!isLocal && !downloading && (
          <TouchableOpacity style={s.actionBtn} onPress={() => doDownload()}>
            <Text style={s.actionTxt}>⬇ Download</Text>
          </TouchableOpacity>
        )}
        {isLocal && (
          <TouchableOpacity style={s.actionBtn} onPress={doExport}>
            <Text style={s.actionTxt}>↗ Teilen</Text>
          </TouchableOpacity>
        )}
        {isLocal && (
          <TouchableOpacity style={[s.actionBtn, { borderColor: C.warn }]} onPress={doDeleteLocal}>
            <Text style={[s.actionTxt, { color: C.warn }]}>Cache löschen</Text>
          </TouchableOpacity>
        )}
      </View>

      {/* Scrub bar */}
      {durMs > 0 && (
        <View style={s.scrubWrap}>
          <View style={s.scrubTrack}>
            <View style={[s.scrubFill, { width: `${(posMs / durMs) * 100}%` }]} />
          </View>
          {/* Tap-to-seek: divide screen width in regions */}
          <View
            style={StyleSheet.absoluteFill}
            onStartShouldSetResponder={() => true}
            onResponderGrant={e => {
              const frac = Math.max(0, Math.min(1, e.nativeEvent.locationX / winW));
              videoRef.current?.setPositionAsync(frac * durMs);
            }}
            onResponderMove={e => {
              const frac = Math.max(0, Math.min(1, e.nativeEvent.locationX / winW));
              videoRef.current?.setPositionAsync(frac * durMs);
            }}
          />
        </View>
      )}

      {/* Info + device delete (small print) */}
      <View style={s.footer}>
        <Text style={s.footerTxt}>
          {isLocal ? '✓ Lokal gespeichert' : '📡 Live stream vom Laptimer'}
        </Text>
        <TouchableOpacity onPress={doDeleteOnDevice}>
          <Text style={s.footerDelete}>Vom Gerät löschen</Text>
        </TouchableOpacity>
      </View>
    </View>
  );
}

const s = StyleSheet.create({
  root:        { flex: 1, backgroundColor: '#000' },
  videoWrap:   { backgroundColor: '#000', justifyContent: 'center', position: 'relative' },
  video:       { width: '100%', height: '100%' },
  lapBar:      { maxHeight: 54, backgroundColor: C.surface, paddingVertical: 8 },
  lapChip:     { paddingHorizontal: 12, paddingVertical: 8, borderRadius: 6,
                 backgroundColor: C.surface2, marginRight: 6 },
  lapChipActive:{ backgroundColor: C.accent },
  lapChipTxt:  { color: C.dim, fontSize: 12, fontWeight: '700' },
  overlayChip: { paddingHorizontal: 12, paddingVertical: 8, borderRadius: 6,
                 backgroundColor: C.surface2, marginRight: 8,
                 borderWidth: 1, borderColor: C.accent },
  overlayChipActive:{ backgroundColor: C.dim },
  overlayChipTxt:{ color: C.accent, fontSize: 12, fontWeight: '700' },
  overlayDownload: {
    ...StyleSheet.absoluteFillObject,
    backgroundColor: 'rgba(0,0,0,0.78)', justifyContent: 'center', alignItems: 'center',
    paddingHorizontal: 40,
  },
  dlTxt:       { color: C.text, marginBottom: 14, fontSize: 15, fontWeight: '700' },
  dlBarTrack:  { width: '100%', height: 8, backgroundColor: C.surface2,
                 borderRadius: 4, overflow: 'hidden', marginBottom: 8 },
  dlBarFill:   { height: '100%', backgroundColor: C.accent },
  dlPctTxt:    { color: C.text, fontSize: 13, fontVariant: ['tabular-nums'],
                 marginBottom: 14 },
  dlCancelBtn: { paddingHorizontal: 20, paddingVertical: 10, borderRadius: 6,
                 borderWidth: 1, borderColor: C.danger, marginTop: 4 },
  dlCancelTxt: { color: C.danger, fontSize: 13, fontWeight: '700' },

  controls:    { flexDirection: 'row', alignItems: 'center',
                 padding: 12, backgroundColor: C.surface, gap: 10 },
  playBtn:     { width: 44, height: 44, borderRadius: 22, backgroundColor: C.accent,
                 justifyContent: 'center', alignItems: 'center' },
  playTxt:     { color: '#000', fontSize: 18, fontWeight: '700' },
  time:        { color: C.text, fontSize: 13, fontVariant: ['tabular-nums'] },
  actionBtn:   { paddingHorizontal: 12, paddingVertical: 8, borderRadius: 6,
                 borderWidth: 1, borderColor: C.accent },
  actionTxt:   { color: C.accent, fontSize: 12, fontWeight: '600' },

  scrubWrap:   { height: 24, backgroundColor: C.surface, justifyContent: 'center',
                 paddingHorizontal: 12 },
  scrubTrack:  { height: 4, backgroundColor: C.surface2, borderRadius: 2, overflow: 'hidden' },
  scrubFill:   { height: '100%', backgroundColor: C.accent },

  footer:      { flexDirection: 'row', justifyContent: 'space-between',
                 padding: 10, backgroundColor: C.surface, borderTopWidth: 1,
                 borderTopColor: C.border },
  footerTxt:   { color: C.dim, fontSize: 11 },
  footerDelete:{ color: C.danger, fontSize: 11, fontWeight: '600' },
});
