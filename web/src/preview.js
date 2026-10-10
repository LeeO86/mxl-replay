// The WebRTC preview (REPLAY_PREVIEW_MODE=webrtc, SPECIFICATION.md §8.6): one WHEP session per page plays
// the mosaic of every channel and camera. Each picture is a <video> on that one MediaStream, cropped to its
// tile with CSS object-view-box (Chrome and Edge 104+; other browsers get the same crop by position). The
// session starts with the first picture, stays for the page, and connects again when it drops.
import { reactive } from "vue";
import { api } from "./api.js";

export const preview = reactive({ map: null, stream: null, connected: false });

const RETRY_MS = 2000;
const viewBox = typeof CSS !== "undefined" && CSS.supports?.("object-view-box", "inset(0px)");
let started = false;

// The own MediaMTX is announced on the replay's address; the page may know the host by another name.
function pageUrl(url, isPublic) {
  if (isPublic) return url;
  try {
    const parsed = new URL(url);
    parsed.hostname = location.hostname;
    return parsed.toString();
  } catch {
    return url;
  }
}

async function connect() {
  let pc = null;
  let done = false;
  const again = () => {
    if (done) return;
    done = true;
    pc?.close();
    preview.connected = false;
    setTimeout(connect, RETRY_MS);
  };
  try {
    if (!preview.map) preview.map = await api.get("/api/v1/preview/map");
    pc = new RTCPeerConnection();
    pc.addTransceiver("video", { direction: "recvonly" });
    const stream = new MediaStream();
    pc.ontrack = (event) => stream.addTrack(event.track);
    await pc.setLocalDescription(await pc.createOffer());
    // The offer goes with all candidates (no trickle).
    await new Promise((resolve) => {
      if (pc.iceGatheringState === "complete") return resolve();
      const timer = setTimeout(resolve, 1000);
      pc.addEventListener("icegatheringstatechange", () => {
        if (pc.iceGatheringState === "complete") {
          clearTimeout(timer);
          resolve();
        }
      });
    });
    const response = await fetch(pageUrl(preview.map.whep, preview.map.public?.whep), {
      method: "POST",
      headers: { "Content-Type": "application/sdp" },
      body: pc.localDescription.sdp,
    });
    if (!response.ok) throw new Error(`WHEP ${response.status}`);
    await pc.setRemoteDescription({ type: "answer", sdp: await response.text() });
    pc.onconnectionstatechange = () => {
      preview.connected = pc.connectionState === "connected";
      if (["failed", "disconnected", "closed"].includes(pc.connectionState)) again();
    };
    preview.stream = stream;
  } catch {
    again();
  }
}

/** Starts the page's WHEP session once. */
export function startPreview() {
  if (started) return;
  started = true;
  connect();
}

/** The style that shows only tile `id` ("ch1", "cam2") of the mosaic in a <video>; null before the map is known. */
export function tileStyle(id) {
  const map = preview.map;
  const tile = map?.tiles.find((t) => t.id === id);
  if (!tile) return null;
  if (viewBox) return { objectViewBox: `inset(${tile.y}px ${map.width - tile.x - tile.w}px ${map.height - tile.y - tile.h}px ${tile.x}px)` };
  // Without object-view-box: the whole mosaic, scaled so that the tile fills the box and moved there.
  return {
    width: `${(map.width / tile.w) * 100}%`,
    height: `${(map.height / tile.h) * 100}%`,
    left: `${(-tile.x / tile.w) * 100}%`,
    top: `${(-tile.y / tile.h) * 100}%`,
    objectFit: "fill",
  };
}
