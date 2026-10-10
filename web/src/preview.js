// The WebRTC preview (REPLAY_PREVIEW_MODE=webrtc, SPECIFICATION.md §8.6): one WHEP session per page plays
// the mosaic of every channel and camera. Each picture is a <video> on that one MediaStream inside a box with
// its tile's aspect ratio and overflow hidden; a CSS transform scales and moves the video so only the tile is
// visible. That works in every browser (object-view-box is Chrome and Edge only). The session starts with the
// first picture, stays for the page, and connects again when it drops.
import { reactive } from "vue";
import { api } from "./api.js";

export const preview = reactive({ map: null, stream: null, connected: false });

const RETRY_MS = 2000;
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

/**
 * How tile `id` ("ch1", "cam2") is shown: `box` for its container (the tile's aspect ratio) and `video` for the
 * <video> filling it, the whole mosaic stretched to the box, moved so the tile is at the top left and scaled up
 * so the tile fills the box. Null before the map is known.
 */
export function tileCrop(id) {
  const map = preview.map;
  const tile = map?.tiles.find((t) => t.id === id);
  if (!tile) return null;
  return {
    box: { aspectRatio: `${tile.w} / ${tile.h}` },
    video: {
      objectFit: "fill",
      transformOrigin: "0 0",
      transform: `scale(${map.width / tile.w}, ${map.height / tile.h}) translate(${(-100 * tile.x) / map.width}%, ${(-100 * tile.y) / map.height}%)`,
    },
  };
}
