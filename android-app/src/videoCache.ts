/**
 * videoCache — small helper around expo-file-system for the per-session
 * video pre-fetch. Mirrors the Studio's auto-download behaviour
 * (tools/brl_studio/ui/analyse_view.py::_AutoDownloadWorker) so the
 * app shows the same "video ready" badge once the cam module's clip
 * has landed locally.
 *
 * VideoScreen had a one-off download path; this module factors it out
 * so DetailScreen can trigger it on mount, the user doesn't have to
 * tap "download" before being able to scrub through a clip.
 */

import * as FileSystem from 'expo-file-system';
import { videoUrl } from './api';

const VIDEOS_DIR = (FileSystem.documentDirectory || '') + 'videos/';

async function ensureDir(): Promise<void> {
  try {
    await FileSystem.makeDirectoryAsync(VIDEOS_DIR, { intermediates: true });
  } catch {
    // Already exists — that's fine
  }
}

export function localVideoPath(videoId: string): string {
  return VIDEOS_DIR + `${videoId}.avi`;
}

export async function isVideoCached(videoId: string): Promise<boolean> {
  const info = await FileSystem.getInfoAsync(localVideoPath(videoId));
  return info.exists && (info as any).size > 0;
}

export interface DownloadProgress {
  bytesWritten: number;
  bytesTotal: number;   // 0 if unknown
}

/**
 * Stream a single video to local cache. Resolves with the local path on
 * success, or rejects with an Error. `onProgress` ticks ~10× per second
 * if the server reports Content-Length.
 */
export async function downloadVideoToCache(
  videoId: string,
  onProgress?: (p: DownloadProgress) => void,
): Promise<string> {
  await ensureDir();
  const dest = localVideoPath(videoId);
  // If a previous attempt left a partial file, expo-file-system's
  // createDownloadResumable rejects without a clean message. Wipe first.
  const info = await FileSystem.getInfoAsync(dest);
  if (info.exists && ((info as any).size ?? 0) === 0) {
    await FileSystem.deleteAsync(dest, { idempotent: true });
  }
  const dl = FileSystem.createDownloadResumable(
    videoUrl(videoId),
    dest,
    {},
    (p: { totalBytesWritten: number; totalBytesExpectedToWrite: number }) => {
      onProgress?.({
        bytesWritten: p.totalBytesWritten,
        bytesTotal: p.totalBytesExpectedToWrite,
      });
    },
  );
  const result = await dl.downloadAsync();
  if (!result || result.status >= 400) {
    throw new Error(`Download failed (HTTP ${result?.status ?? '?'})`);
  }
  return dest;
}

/**
 * Best-effort prefetch: skips silently when the device isn't reachable
 * or the video isn't on the cam (HTTP 404). Used by DetailScreen so
 * opening a session quietly starts pulling its primary video in the
 * background while the user is still looking at the lap list.
 *
 * Returns the local path on success, null when the video wasn't there
 * or any network error occurred. Never throws.
 */
export async function prefetchVideo(
  videoId: string,
  onProgress?: (p: DownloadProgress) => void,
): Promise<string | null> {
  try {
    if (await isVideoCached(videoId)) {
      return localVideoPath(videoId);
    }
    return await downloadVideoToCache(videoId, onProgress);
  } catch {
    return null;
  }
}

export async function deleteCachedVideo(videoId: string): Promise<void> {
  await FileSystem.deleteAsync(localVideoPath(videoId), { idempotent: true });
}
