<script setup>
// Shotbox (SPECIFICATION.md §8.1): one button per clip and playlist. A click cues (yellow), the
// next plays (green, with progress), the next pauses; an ended shot turns grey.
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from "vue";
import ChannelPicker from "./ChannelPicker.vue";
import LivePicture from "./LivePicture.vue";
import Segmented from "./Segmented.vue";
import { api, fmtBytes, fmtNs, fmtSpeed, playNs, serial, SPEED_PRESETS } from "../api.js";
import { act, active, cameraLabel, cameras, channels, channelState, refreshStatus, setSpeed, state, transport } from "../store.js";
import { keyLabel, keymap } from "../keys.js";

const CELL_MIN_WIDTH = 230;
const CELL_HEIGHT = 196;
const GAP = 11; // .7rem

const items = computed(() => {
  const clips = state.clips.map((c) => ({ kind: "clip", id: c.id, name: c.name, colour: c.colour, speed: c.speed, ns: playNs(c), sub: cameraLabel(c.camera), clip: c }));
  const lists = state.playlists.map((p) => ({ kind: "playlist", id: p.id, name: p.name, colour: "#94a3b8", speed: null, ns: p.duration_ns, sub: `${p.entries} clips` }));
  return [...clips, ...lists].sort((a, b) => serial(a.id) - serial(b.id));
});

// Bank size from the space the grid has (§8.1: the grid adapts to the screen).
const area = ref(null);
const columns = ref(5);
const rows = ref(3);
let observer = null;
function measure() {
  if (!area.value) return;
  const { width, height } = area.value.getBoundingClientRect();
  columns.value = Math.max(1, Math.floor((width + GAP) / (CELL_MIN_WIDTH + GAP)));
  rows.value = Math.max(1, Math.floor((height + GAP) / (CELL_HEIGHT + GAP)));
}
const bankSize = computed(() => columns.value * rows.value);
const banks = computed(() => Math.max(1, Math.ceil(items.value.length / bankSize.value)));
const bank = ref(0);
watch(banks, (n) => {
  if (bank.value >= n) bank.value = n - 1;
});
const visible = computed(() => items.value.slice(bank.value * bankSize.value, (bank.value + 1) * bankSize.value));
const bankOptions = computed(() => Array.from({ length: banks.value }, (_, i) => ({ value: i, label: String(i + 1) })));
const selected = ref(-1);

/** The shot's state on the selected channel, and where else it plays. */
function shotState(item) {
  const c = active.value;
  return c && c.shot_id === item.id && !c.live ? c.shot : "idle";
}
function elsewhere(item) {
  return channels.value.filter((c) => c.index !== state.channel && c.shot_id === item.id && !c.live && (c.shot === "playing" || c.shot === "cued"));
}
function progress(item) {
  const c = active.value;
  if (!c || c.shot_id !== item.id || c.out_ns <= c.in_ns) return 0;
  return Math.min(1, Math.max(0, (c.position_ns - c.in_ns) / (c.out_ns - c.in_ns)));
}
const STATE_TEXT = { cued: "CUED", playing: "PLAYING", paused: "PAUSED", ended: "ENDED" };

async function shot(item) {
  await act(() => api.post(`/api/v1/shotbox/${encodeURIComponent(item.id)}`, { channel: state.channel }));
  refreshStatus();
}

function onAction(action, value) {
  if (action === "digit") {
    const index = value === 0 ? 9 : value - 1;
    if (index < visible.value.length) selected.value = index;
    return true;
  }
  if (action === "cue") {
    if (visible.value[selected.value]) shot(visible.value[selected.value]);
    return true;
  }
  if (action === "nextBank" || action === "prevBank") {
    bank.value = Math.min(banks.value - 1, Math.max(0, bank.value + (action === "nextBank" ? 1 : -1)));
    selected.value = -1;
    return true;
  }
  return action === "createClip";
}
defineExpose({ onAction });

const recordingText = computed(() => {
  const wanted = cameras.value.filter((c) => c.record);
  return `Recording ${wanted.filter((c) => c.recording).length}/${wanted.length} cameras`;
});
const speedOptions = SPEED_PRESETS.map((s) => ({ value: s, label: fmtSpeed(s) }));
const currentPreset = computed(() => SPEED_PRESETS.find((s) => Math.abs(s - (active.value?.target_speed ?? 0)) < 0.005) ?? null);

onMounted(async () => {
  await nextTick();
  measure();
  observer = new ResizeObserver(measure);
  if (area.value) observer.observe(area.value);
});
onUnmounted(() => observer?.disconnect());
</script>

<template>
  <div class="shot-top">
    <LivePicture v-if="active" class="target" :src="`/api/v1/channels/${active.index}/preview.jpg`" :interval="500" :bump="state.previewBump" alt="target channel">
      <span class="ov tl"><span class="state-tag" :class="channelState(active)">{{ channelState(active).toUpperCase() }}</span></span>
      <span class="ov bl num">{{ active.timecode }}</span>
    </LivePicture>
    <div class="controls">
      <div class="toolbar" style="margin: 0">
        <ChannelPicker />
      </div>
      <div class="toolbar" style="margin: 0">
        <div class="group">
          <span class="caption">Speed</span>
          <Segmented :options="speedOptions" :model-value="currentPreset" label="Speed preset" @update:model-value="setSpeed" />
        </div>
        <div v-if="banks > 1" class="group">
          <span class="caption">Bank</span>
          <Segmented v-model="bank" :options="bankOptions" label="Bank" />
        </div>
      </div>
    </div>
    <span class="spacer"></span>
    <button class="btn live backlive" @click="transport('live')">Back to live</button>
  </div>

  <div ref="area" class="shot-area">
    <div v-if="!items.length" class="empty">
      No clips yet. Mark IN and OUT on the LSM page and create a clip, or upload a file in the Library.
    </div>
    <div v-else class="shot-grid" :style="{ gridTemplateColumns: `repeat(${columns}, minmax(0, 1fr))`, gridTemplateRows: `repeat(${rows}, ${CELL_HEIGHT}px)` }">
      <button
        v-for="(item, i) in visible"
        :key="item.id"
        class="shot"
        :class="[shotState(item), { selected: i === selected }]"
        :aria-pressed="shotState(item) === 'playing'"
        :title="`${item.name} (${item.id})`"
        @click="selected = i; shot(item)"
      >
        <div class="thumb">
          <img v-if="item.kind === 'clip'" :src="`/api/v1/clips/${encodeURIComponent(item.id)}/thumbnail.jpg?in=${item.clip.in_ns}`" alt="" loading="lazy" />
          <div v-else class="playlist-mark">PLAYLIST</div>
          <span class="key">{{ i < 10 ? (i + 1) % 10 : "" }}</span>
          <span v-if="STATE_TEXT[shotState(item)]" class="badge-state state-tag" :class="shotState(item)">{{ STATE_TEXT[shotState(item)] }}</span>
          <span v-else-if="elsewhere(item).length" class="badge-state state-tag idle">on {{ elsewhere(item).map((c) => c.label).join(", ") }}</span>
        </div>
        <div class="label">
          <div class="name">{{ item.name }}</div>
          <div class="meta">
            <span class="num">{{ fmtNs(item.ns) }}</span>
            <span>{{ item.sub }}</span>
            <span v-if="item.speed != null" class="speed">{{ fmtSpeed(item.speed) }}</span>
            <span v-if="item.kind === 'playlist' && active?.playlist === item.id">entry {{ active.playlist_index + 1 }}</span>
          </div>
        </div>
        <span class="stripe" :style="{ background: item.colour }"></span>
        <span v-if="shotState(item) === 'playing' || shotState(item) === 'paused'" class="progress" :style="{ width: `${progress(item) * 100}%` }"></span>
      </button>
    </div>
  </div>

  <div class="statusline">
    <span>{{ recordingText }}</span>
    <span v-if="state.status">{{ fmtBytes(state.status.free_bytes) }} free</span>
    <span v-if="state.status">JPEG {{ state.status.jpeg }} · {{ state.status.gpu ? `interpolation ${state.status.flow} ${state.status.preset}` : "no interpolation (blend)" }}</span>
    <span class="spacer"></span>
    <span>
      Keys: <kbd>1</kbd>–<kbd>0</kbd> select · <kbd>{{ keyLabel(keymap.cue) }}</kbd> cue / play · <kbd>{{ keyLabel(keymap.playPause) }}</kbd> play / pause ·
      <kbd>{{ keyLabel(keymap.prevBank) }}</kbd> <kbd>{{ keyLabel(keymap.nextBank) }}</kbd> bank · <kbd>{{ keyLabel(keymap.live) }}</kbd> live
    </span>
  </div>
</template>
