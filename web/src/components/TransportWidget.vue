<script setup>
// The transport widget (SPECIFICATION.md §8.7): one channel's state and timecode, Cue (back to its IN,
// paused), Play / Pause, Live (E2E), and its speed (fader 0–200 % and the presets).
import { computed, onUnmounted, ref } from "vue";
import Segmented from "./Segmented.vue";
import { fmtSpeed, SPEED_PRESETS } from "../api.js";
import { channels, channelState, marks, setSpeed, transport } from "../store.js";

const props = defineProps({ channel: { type: Number, required: true } });
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

const presets = SPEED_PRESETS.map((s) => ({ value: s, label: fmtSpeed(s) }));
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
    <div class="btn-grid three">
      <button class="btn secondary" :disabled="!ch.has_in" title="Back to IN, paused" @click="marks('goto-in', ch.index)">Cue</button>
      <button class="btn" :aria-pressed="ch.playing" @click="transport(ch.playing ? 'pause' : 'play', ch.index)">{{ ch.playing ? "Pause" : "Play" }}</button>
      <button class="btn live" :aria-pressed="ch.live" title="Back to live (E2E)" @click="transport('live', ch.index)">Live</button>
    </div>
    <div class="speedrow">
      <input type="range" min="0" max="200" step="1" :value="fader" aria-label="Speed" @input="onInput" @change="onChange" />
      <span class="num">{{ fader }}%</span>
    </div>
    <Segmented :options="presets" :model-value="preset" fullwidth label="Speed preset" @update:model-value="(s) => setSpeed(s, ch.index)" />
  </div>
  <div v-else class="empty">No channel {{ channel }}</div>
</template>
