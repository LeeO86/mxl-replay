<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from "vue";

const page = ref("shotbox");
const status = ref({ cameras: [], channels: [] });
const clips = ref([]);
const playlists = ref([]);
const nmos = ref({});
const configText = ref("");
const channel = ref(1);
const message = ref("");
let socket;
let poll;

const pages = ["shotbox", "lsm", "library", "playlists", "cameras", "nmos", "settings"];

async function api(path, options) {
  const response = await fetch(path, options);
  const type = response.headers.get("content-type") || "";
  const body = type.includes("json") ? await response.json() : await response.text();
  if (!response.ok) {
    throw new Error(body.error || body || response.statusText);
  }
  return body;
}

async function refresh() {
  status.value = await api("/api/v1/status");
  clips.value = await api("/api/v1/clips");
  playlists.value = await api("/api/v1/playlists");
}

function connect() {
  const protocol = location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(`${protocol}://${location.host}/api/v1/events`);
  socket.onmessage = (event) => {
    try {
      status.value = JSON.parse(event.data);
    } catch {
      /* status polls cover a parse miss */
    }
  };
}

const active = computed(() => (status.value.channels || []).find((item) => item.index === channel.value) || {});
const speedPresets = [0.25, 0.33, 0.5, 0.66, 0.75, 1];

async function post(path, body) {
  message.value = "";
  try {
    const result = await api(path, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body || {}),
    });
    if (result.channels) status.value = result;
    await refresh();
  } catch (error) {
    message.value = error.message;
  }
}

async function shot(id) {
  const result = await api(`/api/v1/shotbox/${id}/click`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ channel: channel.value }),
  });
  message.value = result.state;
  await refresh();
}

function onKey(event) {
  if (event.target.matches("input, textarea")) return;
  const number = Number(event.key);
  if (number >= 1 && number <= 9 && clips.value[number - 1]) {
    shot(clips.value[number - 1].id);
  } else if (event.code === "Space") {
    event.preventDefault();
    post(`/api/v1/channels/${channel.value}/transport`, { command: active.value.playing ? "pause" : "play" });
  } else if (event.key === "Enter" && clips.value[0]) {
    shot(clips.value[0].id);
  } else if (event.key === "ArrowLeft" || event.key === "ArrowRight") {
    const step = event.shiftKey ? 0 : event.key === "ArrowLeft" ? -1 : 1;
    if (event.shiftKey) {
      post(`/api/v1/channels/${channel.value}/position`, { seconds: event.key === "ArrowLeft" ? -1 : 1 });
    } else {
      post(`/api/v1/channels/${channel.value}/position`, { frames: step });
    }
  } else if (event.key === "l" || event.key === "L") {
    post(`/api/v1/channels/${channel.value}/transport`, { command: "live" });
  } else if (event.key === "i") {
    post(`/api/v1/channels/${channel.value}/marks`, { which: "in" });
  } else if (event.key === "o") {
    post(`/api/v1/channels/${channel.value}/marks`, { which: "out" });
  }
}

async function createClip() {
  await post("/api/v1/clips", { channel: channel.value, name: "Clip", all_angles: false, force: false });
}

async function uploadFile(event) {
  const file = event.target.files && event.target.files[0];
  if (!file) return;
  const bytes = await file.arrayBuffer();
  message.value = "";
  const response = await fetch("/api/v1/uploads", { method: "POST", body: bytes });
  const body = await response.json();
  message.value = response.ok ? `uploaded ${body.id}` : body.error;
  await refresh();
}

async function loadConfig() {
  configText.value = await api("/api/v1/config/export");
  nmos.value = await api("/api/v1/nmos");
}

async function requestHid() {
  if (!navigator.hid) {
    message.value = "WebHID is not available in this browser";
    return;
  }
  const devices = await navigator.hid.requestDevice({ filters: [] });
  message.value = devices.length ? `HID ${devices[0].productName}` : "no HID device selected";
}

onMounted(async () => {
  await refresh();
  await loadConfig();
  connect();
  poll = setInterval(refresh, 2000);
  window.addEventListener("keydown", onKey);
});
onBeforeUnmount(() => {
  clearInterval(poll);
  if (socket) socket.close();
  window.removeEventListener("keydown", onKey);
});
</script>

<template>
  <div class="app">
    <header>
      <strong>MXL Replay</strong>
      <nav>
        <button v-for="item in pages" :key="item" :class="{ on: page === item }" @click="page = item">{{ item }}</button>
      </nav>
      <label>Channel
        <select v-model.number="channel">
          <option v-for="item in status.channels" :key="item.index" :value="item.index">{{ item.label }}</option>
        </select>
      </label>
      <span class="status">{{ message }}</span>
    </header>

    <section v-if="page === 'shotbox'" class="shotbox">
      <div class="toolbar">
        <img v-if="active.index" :src="`/api/v1/channels/${channel}/preview.jpg?t=${active.position_ns || 0}`" alt="channel preview" />
        <button class="live" @click="post(`/api/v1/channels/${channel}/transport`, { command: 'live' })">Back to live</button>
        <button v-for="speed in speedPresets" :key="speed" @click="post(`/api/v1/channels/${channel}/speed`, { speed })">{{ Math.round(speed * 100) }}%</button>
        <span class="line">Keys: 1–9 shot, Space play/pause, arrows scrub, L live, I/O marks</span>
      </div>
      <div class="grid">
        <button v-for="(clip, index) in clips" :key="clip.id" class="shot" :style="{ borderColor: clip.colour }" @click="shot(clip.id)">
          <b>{{ index + 1 }} {{ clip.name }}</b>
          <small>{{ clip.camera }} · {{ Math.round((clip.speed || 1) * 100) }}%</small>
        </button>
      </div>
      <p class="line">Recording {{ (status.cameras || []).filter((camera) => camera.record).length }} cameras · free {{ status.free_bytes }} · {{ status.preset }}</p>
    </section>

    <section v-else-if="page === 'lsm'" class="lsm">
      <div class="cams">
        <button v-for="camera in status.cameras" :key="camera.index" @click="post(`/api/v1/channels/${channel}/angle`, { camera: camera.index })">
          {{ camera.label }} <i :style="{ background: camera.colour }"></i>
          <small>{{ camera.frames }} frames · missing {{ camera.phase_missing }}</small>
        </button>
      </div>
      <img class="monitor" :src="`/api/v1/channels/${channel}/preview.jpg?t=${active.position_ns || 0}`" alt="monitor" />
      <div class="transport">
        <button @click="post(`/api/v1/channels/${channel}/marks`, { which: 'in' })">IN</button>
        <button @click="post(`/api/v1/channels/${channel}/marks`, { which: 'out' })">OUT</button>
        <button @click="post(`/api/v1/channels/${channel}/marks`, { which: 'goto-in' })">Go IN</button>
        <button @click="post(`/api/v1/channels/${channel}/marks`, { which: 'goto-out' })">Go OUT</button>
        <button @click="post(`/api/v1/channels/${channel}/transport`, { command: 'play' })">Play</button>
        <button @click="post(`/api/v1/channels/${channel}/transport`, { command: 'pause' })">Pause</button>
        <button @click="post(`/api/v1/channels/${channel}/transport`, { command: 'live' })">Live</button>
        <button @click="createClip">Create clip</button>
      </div>
      <label>Speed {{ Math.round((active.speed || 0) * 100) }}%
        <input type="range" min="0" max="200" :value="Math.round((active.speed || 1) * 100)" @change="post(`/api/v1/channels/${channel}/speed`, { speed: $event.target.value / 100 })" />
      </label>
      <div class="transport">
        <button v-for="mode in ['repeat', 'blend', 'interpolate']" :key="mode" @click="post(`/api/v1/channels/${channel}/mode`, { motion: mode })">{{ mode }}</button>
        <button v-for="mode in ['mute', 'stretch', 'follow']" :key="mode" @click="post(`/api/v1/channels/${channel}/mode`, { audio: mode })">audio {{ mode }}</button>
      </div>
      <p>{{ active.label }} cam {{ active.camera }} pos {{ active.position_ns }} {{ active.motion }} {{ active.shot }}</p>
    </section>

    <section v-else-if="page === 'library'">
      <input type="file" @change="uploadFile" />
      <ul>
        <li v-for="clip in clips" :key="clip.id">
          {{ clip.name }} {{ clip.library ? "library" : "buffer" }}
          <button @click="post(`/api/v1/clips/${clip.id}/export`)">Export</button>
          <button @click="post(`/api/v1/clips/${clip.id}/consolidate`)">Consolidate</button>
          <button @click="fetch(`/api/v1/clips/${clip.id}`, { method: 'DELETE' }).then(refresh)">Delete</button>
        </li>
      </ul>
    </section>

    <section v-else-if="page === 'playlists'">
      <button @click="post('/api/v1/playlists', { name: 'Playlist' })">New playlist</button>
      <ul>
        <li v-for="playlist in playlists" :key="playlist.id">
          {{ playlist.name }} ({{ playlist.entries }} entries)
          <button @click="post(`/api/v1/playlists/${playlist.id}/play`, { channel })">Play</button>
        </li>
      </ul>
    </section>

    <section v-else-if="page === 'cameras'">
      <article v-for="camera in status.cameras" :key="camera.index">
        <h3>{{ camera.label }}</h3>
        <p>phases {{ camera.phases }} · HFR ×{{ camera.hfr_factor }} · scaled {{ camera.scaled }} · protected {{ camera.protected_bytes }}</p>
      </article>
    </section>

    <section v-else-if="page === 'nmos'">
      <pre>{{ nmos }}</pre>
    </section>

    <section v-else>
      <textarea :value="configText" readonly rows="16"></textarea>
      <button @click="requestHid">WebHID jog/shuttle</button>
    </section>
  </div>
</template>

<style>
:root { color-scheme: dark; font-family: "Segoe UI", sans-serif; }
body { margin: 0; background: #101218; color: #e8e6e3; }
header, .toolbar, .transport, .cams { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
header { padding: 12px 16px; background: #181b24; position: sticky; top: 0; }
button, select { background: #262b38; color: inherit; border: 1px solid #3a4154; border-radius: 8px; padding: 8px 12px; }
button.on, button.live { background: #1d4ed8; }
section { padding: 16px; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(180px, 1fr)); gap: 12px; }
.shot { min-height: 110px; text-align: left; border-width: 3px; }
.monitor, header img, .toolbar img { width: min(480px, 100%); background: #000; border-radius: 8px; }
textarea { width: min(720px, 100%); background: #0c0e13; color: inherit; }
.line, small { color: #b7b1a8; }
</style>
