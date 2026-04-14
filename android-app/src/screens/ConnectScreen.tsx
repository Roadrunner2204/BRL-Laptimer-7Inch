import React, { useState, useEffect } from 'react';
import {
  View, Text, TextInput, TouchableOpacity, StyleSheet,
  ActivityIndicator, Alert, KeyboardAvoidingView, Platform,
} from 'react-native';
import { NativeStackNavigationProp } from '@react-navigation/native-stack';
import { RootStackParamList } from '../App';
import { fetchDeviceInfo, setBaseUrl } from '../api';
import { loadIp, saveIp } from '../storage';
import { C } from '../theme';

type Props = { navigation: NativeStackNavigationProp<RootStackParamList, 'Connect'> };

export default function ConnectScreen({ navigation }: Props) {
  const [ip, setIp] = useState('192.168.4.1');
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    loadIp().then(setIp);
  }, []);

  async function connect() {
    setLoading(true);
    setBaseUrl(ip);
    let lastErr: unknown;
    // Two attempts: on first connect to a new AP Android sometimes needs
    // a moment to update its routing table.
    for (let attempt = 0; attempt < 2; attempt++) {
      try {
        if (attempt > 0) await new Promise(r => setTimeout(r, 1500));
        await fetchDeviceInfo();
        await saveIp(ip);
        setLoading(false);
        navigation.navigate('Sessions', { mode: 'device' });
        return;
      } catch (e) {
        lastErr = e;
      }
    }
    setLoading(false);
    const errMsg = (lastErr as any)?.message ?? String(lastErr);
    Alert.alert(
      'Verbindungsfehler',
      `Kein BRL Laptimer unter ${ip} erreichbar.\n\n` +
      `Fehlerdetails: ${errMsg}\n\n` +
      `Checkliste:\n` +
      `1. Laptimer eingeschaltet und hochgefahren?\n` +
      `2. Android-WLAN verbunden mit SSID "BRL-Laptimer"?\n` +
      `3. Bei der Nachfrage "Ohne Internet verbunden bleiben?" → JA\n` +
      `4. Mobile Daten während des Verbindens AUS (sonst routet Android dorthin)\n` +
      `5. Netzwerk vergessen und neu verbinden falls immer noch Probleme`,
    );
  }

  return (
    <KeyboardAvoidingView style={s.root} behavior={Platform.OS === 'ios' ? 'padding' : undefined}>
      <View style={s.header}>
        <Text style={s.logo}>BRL</Text>
        <Text style={s.title}>Telemetry</Text>
        <Text style={s.sub}>Laptimer Daten Analyse</Text>
      </View>

      <View style={s.card}>
        <Text style={s.label}>Laptimer IP-Adresse</Text>
        <Text style={s.hint}>AP-Modus: 192.168.4.1  |  Netzwerk: IP aus WLAN-Einstellungen</Text>
        <TextInput
          style={s.input}
          value={ip}
          onChangeText={setIp}
          keyboardType="numeric"
          placeholder="192.168.4.1"
          placeholderTextColor="#555"
          autoCapitalize="none"
          returnKeyType="done"
          onSubmitEditing={connect}
        />
        <TouchableOpacity style={s.btn} onPress={connect} disabled={loading}>
          {loading
            ? <ActivityIndicator color="#000" />
            : <Text style={s.btnTxt}>Verbinden</Text>}
        </TouchableOpacity>
      </View>

      <TouchableOpacity style={s.localBtn} onPress={() => navigation.navigate('Sessions', { mode: 'local' })}>
        <Text style={s.localTxt}>Gespeicherte Sessions anzeigen</Text>
      </TouchableOpacity>
    </KeyboardAvoidingView>
  );
}

const s = StyleSheet.create({
  root:     { flex:1, backgroundColor: C.bg, justifyContent:'center', padding:24 },
  header:   { alignItems:'center', marginBottom:48 },
  logo:     { fontSize:52, fontWeight:'900', color: C.accent, letterSpacing:4 },
  title:    { fontSize:28, fontWeight:'700', color: C.text, marginTop:-8 },
  sub:      { fontSize:13, color: C.dim, marginTop:4 },
  card:     { backgroundColor: C.surface, borderRadius:12, padding:20 },
  label:    { color: C.text, fontSize:14, fontWeight:'600', marginBottom:4 },
  hint:     { color: C.dim, fontSize:11, marginBottom:12 },
  input:    { backgroundColor: C.bg, borderRadius:8, padding:12, color: C.text,
               fontSize:18, borderWidth:1, borderColor:'#333', marginBottom:16 },
  btn:      { backgroundColor: C.accent, borderRadius:8, padding:14, alignItems:'center' },
  btnTxt:   { color:'#000', fontWeight:'700', fontSize:16 },
  localBtn: { marginTop:20, alignItems:'center' },
  localTxt: { color: C.dim, fontSize:14, textDecorationLine:'underline' },
});
