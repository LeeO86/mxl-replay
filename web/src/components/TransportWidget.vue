<script setup>
// The transport widget (SPECIFICATION.md §8.7): one channel's state and timecode, the countdowns to the clip's
// OUT and the playlist's end, Cue (back to its IN, paused), Play / Pause, Stop (the idle playlist), Live (E2E),
// and its speed (fader 0–200 % and, with presets (default), the presets). With clip=true: IN, OUT, Go IN, Go OUT and
// Create clip (the channel's IN to OUT, named automatically).
import { computed, onUnmounted, ref } from "vue";
import Segmented from "./Segmented.vue";
import { api, fmtNs, fmtSpeed, SPEED_PRESETS } from "../api.js";
import { channels, channelState, marks, refreshLists, setSpeed, state, transport } from "../store.js";

const props = defineProps({
  channel: { type: Number, required: true },
  clip: { type: Boolean, default: false },
  presets: { type: Boolean, default: true },
});
const ch = computed(() => channels.value.find((c) => c.index === props.channel) || null);

// The fader shows the speed set on the channel, or its own value while it is dragged.
const target = computed(() => ch.value?.target_speed ?? ch.value?.speed ?? 0);
const drag = ref(null);
const fader = computed(() => drag.value ?? Math.round(Math.abs(target.value) * 100));
let timer = null;
function onInput(event) {
  drag.value = Number(event.target.value);
  if (!timer) {
    timer = setTimeout(() => {
      timer = null;
      if (drag.value != null) setSpeed(drag.value / 100, props.channel);
    }, 80);
  }
}
async function onChange(event) {
  clearTimeout(timer);
  timer = null;
  await setSpeed(Number(event.target.value) / 100, props.channel);
  drag.value = null;
}
onUnmounted(() => clearTimeout(timer));

const canCreate = computed(() => ch.value?.has_in && ch.value?.has_out && ch.value.out_ns > ch.value.in_ns);
const creating = ref(false);
const created = ref(""); // the last clip made here
// One click: the clip gets its automatic name. When clips already keep too much of the storage (409) the operator confirms.
async function createClip(force = false) {
  creating.value = true;
  try {
    const clip = await api.post("/api/v1/clips", { channel: props.channel, force });
    state.error = "";
    created.value = clip.id;
    await refreshLists();
  } catch (e) {
    if (e.status === 409 && !force && confirm(`${e.message}\n\nCreate the clip anyway?`)) return createClip(true);
    state.error = e.message;
  } finally {
    creating.value = false;
  }
}

const presetOptions = SPEED_PRESETS.map((s) => ({ value: s, label: fmtSpeed(s) }));
const preset = computed(() => SPEED_PRESETS.find((s) => Math.abs(s - Math.abs(target.value)) < 0.005) ?? null);
</script>

<template>
  <div v-if="ch" class="widget-transport">
    <div class="widget-head">
      <span class="name">{{ ch.label }}</span>
      <span class="state-tag" :class="channelState(ch)">{{ channelState(ch).toUpperCase() }}</span>
      <span class="spacer"></span>
      <span class="tc">{{ ch.timecode || "--:--:--:--" }}</span>
      <span class="num">{{ fmtSpeed(ch.speed) }}</span>
    </div>
    <div class="widget-cd muted small">
      <span>Clip <span class="num">{{ ch.clip_remaining_ns == null ? "–" : `−${fmtNs(ch.clip_remaining_ns)}` }}</span></span>
      <span>Playlist <span class="num">{{ ch.playlist_remaining_ns == null ? "–" : `−${fmtNs(ch.playlist_remaining_ns)}` }}</span></span>
      <span v-if="ch.idle">idle</span>
    </div>
    <div class="btn-grid four">
      <button class="btn secondary" :disabled="!ch.has_in" title="Back to IN, paused" @click="marks('goto-in', ch.index)">Cue</button>
      <button class="btn" :aria-pressed="ch.playing" @click="transport(ch.playing ? 'pause' : 'play', ch.index)">{{ ch.playing ? "Pause" : "Play" }}</button>
      <button class="btn secondary" :title="ch.idle_playlist ? 'Stop: the idle playlist plays' : 'Stop: the shot ends where it stands'" @click="transport('stop', ch.index)">Stop</button>
      <button class="btn live" :aria-pressed="ch.live" title="Back to live (E2E)" @click="transport('live', ch.index)">Live</button>
    </div>
    <template v-if="clip">
      <div class="btn-grid four">
        <button class="btn mark-in" title="Mark IN at the current position" @click="marks('in', ch.index)">IN</button>
        <button class="btn mark-out" title="Mark OUT at the current position" @click="marks('out', ch.index)">OUT</button>
        <button class="btn secondary" :disabled="!ch.has_in" @click="marks('goto-in', ch.index)">Go IN</button>
        <button class="btn secondary" :disabled="!ch.has_out" @click="marks('goto-out', ch.index)">Go OUT</button>
      </div>
      <div class="widget-marks small">
        <span><span class="muted">IN</span> <span class="mono">{{ ch.has_in ? ch.in_tc : "–" }}</span></span>
        <span><span class="muted">OUT</span> <span class="mono">{{ ch.has_out ? ch.out_tc : "–" }}</span></span>
        <span><span class="muted">Length</span> <span class="num">{{ canCreate ? fmtNs(ch.out_ns - ch.in_ns) : "–" }}</span></span>
        <span class="spacer"></span>
        <span v-if="created" class="pill ok" title="the last clip created here">{{ created }}</span>
        <button class="btn small" :disabled="!canCreate || creating" :title="canCreate ? 'IN to OUT as a new clip' : 'Mark IN and OUT first'" @click="createClip()">
          Create clip
        </button>
      </div>
    </template>
    <div class="speedrow">
      <input type="range" min="0" max="200" step="1" :value="fader" aria-label="Speed" @input="onInput" @change="onChange" />
      <span class="num">{{ fader }}%</span>
    </div>
    <Segmented v-if="presets" :options="presetOptions" :model-value="preset" fullwidth label="Speed preset" @update:model-value="(s) => setSpeed(s, ch.index)" />
  </div>
  <div v-else class="empty">No channel {{ channel }}</div>
</template>
