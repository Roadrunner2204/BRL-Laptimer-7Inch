import { requireNativeModule } from 'expo-modules-core';

const WifiBinder = requireNativeModule('WifiBinder');

/** Route all process sockets through WiFi. Call before fetch to the AP. */
export async function bindToWifi(): Promise<boolean> {
  return WifiBinder.bindToWifi();
}

/** Release WiFi binding so normal internet routing resumes. */
export async function unbindFromWifi(): Promise<void> {
  return WifiBinder.unbind();
}
