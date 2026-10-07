// Shared UI state: the status from the WebSocket (applied at most 10 times a second), the clip
// and playlist lists, the selected channel, and the actions every page uses.
import { computed, reactive } from "vue";
import { api, ApiError, serial } from "./api.js";

function storedChannel() {
  try {
    return Number(localStorage.getItem("mxl-replay.channel")) || 1;
  } catch {
    return 1;
  }
}

export const state = reactive({
  status: null,
  clips: [],
  playlists: [],
  channel: storedChannel(),
  connected: false, // WebSocket /api/v1/events
  indexing: false, // 503 {"status":"indexing"} while the buffer is indexed
  statusError: "",
  error: "", // the last failed action (banner)
  notice: "", // the last action's result worth showing (banner)
  restartRequired: false,
  previewBump: 0, // changes when the channel picture changes at once (angle, mode)
});

export const channels = computed(() => state.status?.channels || []);
export const cameras = computed(() => state.status?.cameras || []);
export const active = computed(() => channels.value.find((c) => c.index === state.channel) || channels.value[0] || null);
export const clipsById = computed(() => Object.fromEntries(state.clips.map((c) => [c.id, c])));
export const cameraLabel = (index) => (index === 0 ? "Library" : cameras.value.find((c) => c.index === index)?.label || `Cam ${index}`);
export const cameraColour = (index) => cameras.value.find((c) => c.index === index)?.colour || "#94a3b8";
/** Frame period of a camera (house period / HFR factor); uploads play at the house rate. */
export const framePeriod = (cameraIndex) => {
  const house = state.status?.frame_ns || 20000000;
  const factor = cameras.value.find((c) => c.index === cameraIndex)?.hfr_factor || 1;
  return house / Math.max(1, factor);
};

export function selectChannel(index) {
  state.channel = index;
  try {
    localStorage.setItem("mxl-replay.channel", String(index));
  } catch {
    /* the selection is only kept for this tab */
  }
}

/** Channel state shown on buttons and the monitor: live, playing, cued, paused, ended, black. */
export function channelState(c) {
  if (!c) return "idle";
  if (c.live) return "live";
  if (c.black) return "black";
  if (c.playing) return "playing";
  if (c.shot === "cued" || c.shot === "ended") return c.shot;
  return "paused";
}

function applyStatus(status) {
  state.status = status;
  state.indexing = false;
  state.statusError = "";
}

export async function refreshStatus() {
  try {
    applyStatus(await api.get("/api/v1/status"));
  } catch (e) {
    if (e instanceof ApiError && e.status === 503 && e.body?.status === "indexing") {
      state.indexing = true;
      state.statusError = "";
    } else {
      state.statusError = `Status unavailable: ${e.message}`;
    }
  }
}

export async function refreshLists() {
  const [clips, playlists] = await Promise.allSettled([api.get("/api/v1/clips"), api.get("/api/v1/playlists")]);
  if (clips.status === "fulfilled") state.clips = clips.value.sort((a, b) => serial(a.id) - serial(b.id));
  if (playlists.status === "fulfilled") state.playlists = playlists.value.sort((a, b) => serial(a.id) - serial(b.id));
}

/** Runs an action; a failure goes to the error banner. Returns the result, or undefined. */
export async function act(fn) {
  try {
    const result = await fn();
    state.error = "";
    return result;
  } catch (e) {
    state.error = e.message;
    return undefined;
  }
}

/** POST /api/v1/channels/{n}/{action}; the answer is the new status. */
export function channelPost(action, body, channel = state.channel) {
  return act(async () => {
    const status = await api.post(`/api/v1/channels/${channel}/${action}`, body);
    applyStatus(status);
    return status;
  });
}

export const transport = (command, channel) => channelPost("transport", { command }, channel);
export const marks = (which, channel) => channelPost("marks", { which }, channel);
export const setSpeed = (speed, channel) => channelPost("speed", { speed: Math.max(-1, Math.min(2, Math.round(speed * 100) / 100)) }, channel);
export const seek = (taiNs, channel) => channelPost("position", { tai_ns: Math.max(0, Math.round(taiNs)) }, channel);
export const scrubSeconds = (seconds, channel) => channelPost("position", { seconds }, channel);
export async function setAngle(camera, channel) {
  await channelPost("angle", { camera }, channel);
  state.previewBump++;
}
export async function setMode(body, channel) {
  await channelPost("mode", body, channel);
  state.previewBump++;
}

// Frame steps from the wheel and the jog wheel: added up while a request is on its way.
let scrubPending = 0;
let scrubBusy = false;
export async function scrubFrames(frames) {
  scrubPending += frames;
  if (scrubBusy) return;
  scrubBusy = true;
  while (scrubPending !== 0) {
    const n = scrubPending;
    scrubPending = 0;
    await channelPost("position", { frames: n });
  }
  scrubBusy = false;
}

export function togglePlay() {
  return transport(active.value?.playing ? "pause" : "play");
}

export function nudgeSpeed(delta) {
  const current = active.value?.target_speed ?? 1;
  return setSpeed(Math.max(0, Math.min(2, Math.round(current * 100 + delta) / 100)));
}

let socket = null;
let pending = null;
let retry = 1000;
let applier = null;
let poller = null;
let lister = null;

function connect() {
  const protocol = location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(`${protocol}://${location.host}/api/v1/events`);
  socket.onopen = () => {
    state.connected = true;
    retry = 1000;
  };
  // The server pushes the status every frame (50/s): keep the newest, render at 10 Hz.
  socket.onmessage = (event) => {
    pending = event.data;
  };
  socket.onclose = () => {
    state.connected = false;
    socket = null;
    setTimeout(connect, retry);
    retry = Math.min(retry * 2, 5000);
  };
}

export function start() {
  refreshStatus();
  refreshLists();
  connect();
  applier = setInterval(() => {
    if (pending == null) return;
    const text = pending;
    pending = null;
    try {
      applyStatus(JSON.parse(text));
    } catch {
      /* the next push replaces it */
    }
  }, 100);
  // Without the WebSocket (or while indexing) the status is polled.
  poller = setInterval(() => {
    if (!state.connected || state.indexing || !state.status) refreshStatus();
  }, 1000);
  // Other UIs and API clients change clips and playlists too.
  lister = setInterval(refreshLists, 5000);
}

export function stop() {
  clearInterval(applier);
  clearInterval(poller);
  clearInterval(lister);
  if (socket) {
    socket.onclose = null;
    socket.close();
  }
}
