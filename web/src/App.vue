<script setup>
import { computed, onMounted, onUnmounted, ref } from "vue";
import Pill from "./components/Pill.vue";
import ShotboxTab from "./components/ShotboxTab.vue";
import LsmTab from "./components/LsmTab.vue";
import LibraryTab from "./components/LibraryTab.vue";
import PlaylistsTab from "./components/PlaylistsTab.vue";
import CamerasTab from "./components/CamerasTab.vue";
import NmosTab from "./components/NmosTab.vue";
import SettingsTab from "./components/SettingsTab.vue";
import { fmtBytes } from "./api.js";
import { actionFor, comboOf } from "./keys.js";
import { onHidAction, restoreHid, SHUTTLE_SPEEDS } from "./hid.js";
import {
  active,
  cameras,
  channels,
  marks,
  nudgeSpeed,
  scrubFrames,
  scrubSeconds,
  setAngle,
  setSpeed,
  start,
  state,
  stop,
  togglePlay,
  transport,
} from "./store.js";

const tabs = [
  { id: "shotbox", label: "Shotbox", component: ShotboxTab, fill: true },
  { id: "lsm", label: "LSM", component: LsmTab, fill: true },
  { id: "library", label: "Library", component: LibraryTab },
  { id: "playlists", label: "Playlists", component: PlaylistsTab },
  { id: "cameras", label: "Cameras", component: CamerasTab },
  { id: "nmos", label: "NMOS", component: NmosTab },
  { id: "settings", label: "Settings", component: SettingsTab },
];

const current = ref("shotbox");
const tab = computed(() => tabs.find((t) => t.id === current.value));
const page = ref(null);

function onHash() {
  const id = location.hash.slice(1);
  if (tabs.some((t) => t.id === id)) current.value = id;
}
function go(id) {
  current.value = id;
  location.hash = id;
}

const status = computed(() => state.status);
const recording = computed(() => {
  const wanted = cameras.value.filter((c) => c.record);
  const ok = wanted.filter((c) => c.recording).length;
  return { ok, total: wanted.length, kind: ok === wanted.length ? "ok" : ok === 0 ? "bad" : "warn" };
});
const lowDisk = computed(() => (status.value?.free_bytes ?? Infinity) < 20 * 1024 ** 3);
const gpuText = computed(() => {
  if (!status.value) return "";
  if (status.value.jpeg === "nvjpeg") return `GPU · ${status.value.flow} ${status.value.preset}`;
  return status.value.gpu ? "CPU JPEG" : "CPU · no interpolation";
});
const identity = computed(() => {
  const s = status.value;
  if (!s) return "";
  return `${s.label} · ${s.format} · ${cameras.value.length} cameras · ${channels.value.length} channels`;
});

// Actions from the keyboard and the jog/shuttle. The page gets the first look (digits,
// Enter, banks, create clip); the rest works on the selected channel.
function dispatch(action, value) {
  if (page.value?.onAction?.(action, value)) return;
  const camera = /^camera(\d+)$/.exec(action);
  const speed = /^speed(\d+)$/.exec(action);
  if (camera) return setAngle(Number(camera[1]));
  if (speed) return setSpeed(Number(speed[1]) / 100);
  switch (action) {
    case "playPause":
      return togglePlay();
    case "live":
      return transport("live");
    case "markIn":
      return marks("in");
    case "markOut":
      return marks("out");
    case "gotoIn":
      return marks("goto-in");
    case "gotoOut":
      return marks("goto-out");
    case "frameBack":
      return scrubFrames(-1);
    case "frameForward":
      return scrubFrames(1);
    case "secondBack":
      return scrubSeconds(-1);
    case "secondForward":
      return scrubSeconds(1);
    case "speedUp":
      return nudgeSpeed(5);
    case "speedDown":
      return nudgeSpeed(-5);
    case "jog":
      return scrubFrames(value);
    case "shuttle": {
      if (value === 0) return transport("pause");
      const s = Math.sign(value) * SHUTTLE_SPEEDS[Math.min(7, Math.abs(value))];
      return setSpeed(Math.max(-1, s)).then(() => !active.value?.playing && transport("play"));
    }
  }
}

function onKey(event) {
  if (event.defaultPrevented || event.isComposing || !tab.value.fill) return;
  const target = event.target;
  // Typing in a field, or a dialog: not for the transport.
  if (target.closest?.("input, select, textarea, [contenteditable], dialog")) return;
  const combo = comboOf(event);
  // A focused button takes Space and Enter itself.
  if (target.closest?.("button") && (combo === "Space" || combo === "Enter")) return;
  if (/^[0-9]$/.test(event.key) && !event.ctrlKey && !event.altKey && !event.metaKey) {
    event.preventDefault();
    dispatch("digit", Number(event.key));
    return;
  }
  const action = actionFor(combo);
  if (!action) return;
  event.preventDefault();
  dispatch(action);
}

// A mouse click leaves no focus on the button, so Space stays play/pause.
function onClick(event) {
  if (event.detail === 0) return;
  const button = event.target.closest?.("button");
  if (button && !button.closest("dialog")) button.blur();
}

onMounted(() => {
  onHash();
  window.addEventListener("hashchange", onHash);
  window.addEventListener("keydown", onKey);
  document.addEventListener("click", onClick);
  start();
  onHidAction((action, value) => dispatch(action, value));
  restoreHid();
});
onUnmounted(() => {
  window.removeEventListener("hashchange", onHash);
  window.removeEventListener("keydown", onKey);
  document.removeEventListener("click", onClick);
  stop();
});
</script>

<template>
  <div class="shell" :class="{ fill: tab.fill }">
    <header>
      <h1>mxl-replay</h1>
      <span v-if="identity" class="node">{{ identity }}</span>
      <span class="spacer"></span>
      <template v-if="status">
        <Pill :text="`REC ${recording.ok}/${recording.total}`" :kind="recording.kind" title="cameras recording" />
        <Pill :text="`${fmtBytes(status.free_bytes)} free`" :kind="lowDisk ? 'warn' : 'neutral'" title="free space on the buffer storage" />
        <Pill :text="gpuText" :kind="status.jpeg === 'nvjpeg' ? 'ok' : 'warn'" title="JPEG codec and interpolation" />
      </template>
      <Pill :text="state.connected ? 'live' : 'offline'" :kind="state.connected ? 'ok' : 'bad'" title="/api/v1/events" />
      <span v-if="status" class="muted small">v{{ status.version }} · MXL {{ status.mxl.slice(0, 7) }} · nmos-cpp {{ status.nmos_cpp.slice(0, 7) }}</span>
    </header>
    <div v-if="state.indexing" class="banner info">Indexing the buffer… the replay answers as soon as the stored segments are indexed.</div>
    <div v-else-if="state.statusError" class="banner bad">{{ state.statusError }}</div>
    <div v-else-if="status && !state.connected" class="banner warn">Live updates lost — reconnecting…</div>
    <div v-if="state.error" class="banner bad">
      {{ state.error }}
      <span class="spacer"></span>
      <button class="btn small secondary" @click="state.error = ''">Dismiss</button>
    </div>
    <div v-if="state.restartRequired" class="banner warn">Settings imported: restart the replay to apply ports, format, counts, seed or domain.</div>
    <div v-if="state.notice" class="banner info">
      {{ state.notice }}
      <span class="spacer"></span>
      <button class="btn small secondary" @click="state.notice = ''">Dismiss</button>
    </div>
    <nav>
      <button v-for="t in tabs" :key="t.id" :class="{ active: current === t.id }" :aria-current="current === t.id ? 'page' : undefined" @click="go(t.id)">
        {{ t.label }}
      </button>
    </nav>
    <main :class="{ fill: tab.fill }">
      <component :is="tab.component" ref="page" />
    </main>
  </div>
</template>
