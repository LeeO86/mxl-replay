<script setup>
// The buffer of the channel's camera: clips, IN/OUT and the play position. Drag to scrub,
// wheel ±1 frame (Shift ±1 s). Labels count back from the newest recorded frame.
import { computed, ref } from "vue";
import Segmented from "./Segmented.vue";
import { fmtNs } from "../api.js";
import { active, cameraLabel, cameras, scrubFrames, scrubSeconds, seek, state } from "../store.js";

const ZOOMS = [
  { value: 30, label: "30 s" },
  { value: 120, label: "2 min" },
  { value: 600, label: "10 min" },
  { value: 0, label: "All" },
];
const zoom = ref(120);
const track = ref(null);
const dragTai = ref(null);

const camera = computed(() => cameras.value.find((c) => c.index === active.value?.camera));
const range = computed(() => {
  const c = active.value;
  if (!c) return null;
  const cam = camera.value;
  let first;
  let last;
  if (cam && cam.newest_ns) {
    last = cam.newest_ns;
    first = Math.max(cam.oldest_ns, last - cam.buffer_hours * 3600e9);
  } else {
    // An upload plays from the library: its own clip is the range.
    first = c.in_ns || c.position_ns;
    last = Math.max(c.out_ns || 0, first + 1e9);
  }
  const span = zoom.value * 1e9;
  let start = span ? Math.max(first, last - span) : first;
  let end = last;
  const pos = dragTai.value ?? c.position_ns;
  if (span && pos < start) {
    start = Math.max(first, pos - span * 0.25);
    end = start + span;
  }
  if (end - start < 1e9) end = start + 1e9;
  return { first, last, start, end };
});

const x = (ns) => ((ns - range.value.start) / (range.value.end - range.value.start)) * 100;
const clamp = (v) => Math.min(100, Math.max(0, v));
const position = computed(() => dragTai.value ?? active.value?.position_ns ?? 0);

const clipMarks = computed(() => {
  const r = range.value;
  if (!r) return [];
  const cam = active.value.camera;
  return state.clips
    .filter((c) => (cam === 0 ? c.camera === 0 : c.camera !== 0) && c.out_ns >= r.start && c.in_ns <= r.end)
    .map((c) => ({
      id: c.id,
      title: `${c.name} (${cameraLabel(c.camera)})`,
      colour: c.colour,
      other: c.camera !== cam,
      left: clamp(x(c.in_ns)),
      width: Math.max(0.25, clamp(x(c.out_ns)) - clamp(x(c.in_ns))),
    }));
});

const ticks = computed(() => {
  const r = range.value;
  if (!r) return [];
  const span = (r.end - r.start) / 1e9;
  const step = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600].find((s) => span / s <= 10) || 7200;
  const out = [];
  for (let k = 0; k < 60; ++k) {
    const t = r.last - k * step * 1e9;
    if (t < r.start) break;
    if (t > r.end) continue;
    const secs = k * step;
    const label = k === 0 ? "newest" : `−${Math.floor(secs / 60)}:${String(secs % 60).padStart(2, "0")}`;
    out.push({ left: x(t), label });
  }
  return out;
});

// One seek on its way at a time; the newest drag position wins.
let sending = false;
let queued = null;
async function send(tai) {
  queued = tai;
  if (sending) return;
  sending = true;
  while (queued != null) {
    const t = queued;
    queued = null;
    await seek(t);
  }
  sending = false;
}
function taiAt(event) {
  const rect = track.value.getBoundingClientRect();
  const f = Math.min(1, Math.max(0, (event.clientX - rect.left) / rect.width));
  return range.value.start + f * (range.value.end - range.value.start);
}
function down(event) {
  if (event.button !== 0) return;
  track.value.setPointerCapture(event.pointerId);
  dragTai.value = taiAt(event);
  send(dragTai.value);
}
function move(event) {
  if (dragTai.value == null) return;
  dragTai.value = taiAt(event);
  send(dragTai.value);
}
function up() {
  dragTai.value = null;
}
function wheel(event) {
  const d = event.deltaY || event.deltaX;
  if (!d) return;
  if (event.shiftKey) scrubSeconds(d > 0 ? 1 : -1);
  else scrubFrames(d > 0 ? 1 : -1);
}
</script>

<template>
  <div v-if="range">
    <div class="timeline-head">
      <strong>Timeline</strong>
      <span class="muted">{{ cameraLabel(active.camera) }} · {{ fmtNs(range.last - range.first) }} in the buffer</span>
      <span class="spacer"></span>
      <span class="muted small">Drag to scrub · wheel ±1 frame, Shift ±1 s</span>
      <Segmented v-model="zoom" :options="ZOOMS" label="Zoom" />
    </div>
    <div
      ref="track"
      class="timeline"
      role="slider"
      aria-label="Position in the buffer"
      :aria-valuetext="active.position_tc"
      @pointerdown="down"
      @pointermove="move"
      @pointerup="up"
      @pointercancel="up"
      @wheel.prevent="wheel"
    >
      <div class="buffer" :style="{ left: `${clamp(x(range.first))}%`, width: `${clamp(x(range.last)) - clamp(x(range.first))}%` }"></div>
      <div
        v-if="active.has_in && active.has_out && active.out_ns > active.in_ns"
        class="range"
        :style="{ left: `${clamp(x(active.in_ns))}%`, width: `${clamp(x(active.out_ns)) - clamp(x(active.in_ns))}%` }"
      ></div>
      <div v-for="m in clipMarks" :key="m.id" class="clipmark" :class="{ other: m.other }" :style="{ left: `${m.left}%`, width: `${m.width}%`, background: m.colour }" :title="m.title"></div>
      <div v-if="active.has_in && x(active.in_ns) >= 0 && x(active.in_ns) <= 100" class="mark in" :style="{ left: `${x(active.in_ns)}%` }"><span>IN</span></div>
      <div v-if="active.has_out && x(active.out_ns) >= 0 && x(active.out_ns) <= 100" class="mark out" :style="{ left: `${x(active.out_ns)}%` }"><span>OUT</span></div>
      <div class="head" :style="{ left: `${clamp(x(position))}%` }"></div>
      <div v-for="t in ticks" :key="t.label" class="tick" :class="{ end: t.left > 92 }" :style="{ left: `${t.left}%` }">{{ t.label }}</div>
    </div>
  </div>
</template>
