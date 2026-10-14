<script setup>
// The timeline widget (SPECIFICATION.md §8.7): the LSM tab's Timeline for one channel (its camera's buffer, the clips
// as marks, IN/OUT, the play position): drag to seek, wheel ±1 frame (Shift ±1 s); IN, OUT, Go IN, Go OUT and ±1 frame.
// The widget page selects `channel` (not kept), so the Timeline and the scrub keys work on it.
import { computed } from "vue";
import Timeline from "./Timeline.vue";
import { fmtNs } from "../api.js";
import { channels, channelState, marks, scrubFrames, transport } from "../store.js";

const props = defineProps({ channel: { type: Number, required: true } });
const ch = computed(() => channels.value.find((c) => c.index === props.channel) || null);
const length = computed(() => (ch.value?.has_in && ch.value?.has_out && ch.value.out_ns > ch.value.in_ns ? ch.value.out_ns - ch.value.in_ns : null));
</script>

<template>
  <div v-if="ch" class="widget-timeline">
    <div class="widget-head">
      <span class="name">{{ ch.label }}</span>
      <span class="state-tag" :class="channelState(ch)">{{ channelState(ch).toUpperCase() }}</span>
      <span class="tc">{{ ch.timecode || "--:--:--:--" }}</span>
      <span class="spacer"></span>
      <span class="widget-marks small">
        <span><span class="muted">IN</span> <span class="mono">{{ ch.has_in ? ch.in_tc : "–" }}</span></span>
        <span><span class="muted">OUT</span> <span class="mono">{{ ch.has_out ? ch.out_tc : "–" }}</span></span>
        <span><span class="muted">Length</span> <span class="num">{{ length == null ? "–" : fmtNs(length) }}</span></span>
      </span>
    </div>
    <Timeline />
    <div class="btn-grid" style="grid-template-columns: repeat(7, 1fr)">
      <button class="btn mark-in" title="Mark IN at the current position" @click="marks('in', ch.index)">IN</button>
      <button class="btn mark-out" title="Mark OUT at the current position" @click="marks('out', ch.index)">OUT</button>
      <button class="btn secondary" :disabled="!ch.has_in" @click="marks('goto-in', ch.index)">Go IN</button>
      <button class="btn secondary" :disabled="!ch.has_out" @click="marks('goto-out', ch.index)">Go OUT</button>
      <button class="btn secondary" @click="scrubFrames(-1)">−1 f</button>
      <button class="btn secondary" @click="scrubFrames(1)">+1 f</button>
      <button class="btn" :aria-pressed="ch.playing" @click="transport(ch.playing ? 'pause' : 'play', ch.index)">{{ ch.playing ? "Pause" : "Play" }}</button>
    </div>
  </div>
  <div v-else class="empty">No channel {{ channel }}</div>
</template>
