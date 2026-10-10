<script setup>
// The clip list widget (SPECIFICATION.md §8.7): every clip, newest first, with its button on the channel:
// Load cues it on its IN (a shotbox click; on a cued or paused clip the next click plays, on a playing
// one it pauses). Without `channel` the widget has a channel picker.
import { computed } from "vue";
import ChannelPicker from "./ChannelPicker.vue";
import { api, fmtNs, fmtSpeed, playNs } from "../api.js";
import { act, cameraLabel, channels, refreshStatus, state } from "../store.js";

const props = defineProps({ channel: { type: Number, default: 0 } });
const target = computed(() => channels.value.find((c) => c.index === (props.channel || state.channel)) || null);
const clips = computed(() => [...state.clips].reverse());

function shotState(clip) {
  const c = target.value;
  return c && c.shot_id === clip.id && !c.live ? c.shot : "idle";
}
const ACTION = { idle: "Load", ended: "Load", cued: "Play", paused: "Play", playing: "Pause" };
const STATE_TEXT = { cued: "CUED", playing: "PLAYING", paused: "PAUSED", ended: "ENDED" };

async function click(clip) {
  await act(() => api.post(`/api/v1/shotbox/${encodeURIComponent(clip.id)}`, { channel: target.value.index }));
  refreshStatus();
}
</script>

<template>
  <div class="widget-clips">
    <div v-if="!channel" class="toolbar" style="margin: 0"><ChannelPicker /></div>
    <div v-else-if="target" class="widget-head">
      <span class="muted">Load into</span><span class="name">{{ target.label }}</span>
    </div>
    <div class="widget-list">
      <div v-if="!clips.length" class="empty">No clips yet.</div>
      <div v-for="clip in clips" :key="clip.id" class="widget-clip" :class="shotState(clip)">
        <span class="stripe" :style="{ background: clip.colour }"></span>
        <img :src="`/api/v1/clips/${encodeURIComponent(clip.id)}/thumbnail.jpg?in=${clip.in_ns}`" alt="" loading="lazy" @error="$event.target.style.visibility = 'hidden'" />
        <div class="text">
          <div class="name" :title="`${clip.name} (${clip.id})`">{{ clip.name }}</div>
          <div class="meta">
            <span class="num">{{ fmtNs(playNs(clip)) }}</span> · {{ cameraLabel(clip.camera) }} · {{ fmtSpeed(clip.speed) }}
          </div>
        </div>
        <span v-if="STATE_TEXT[shotState(clip)]" class="state-tag" :class="shotState(clip)">{{ STATE_TEXT[shotState(clip)] }}</span>
        <button class="btn small" :disabled="!target" @click="click(clip)">{{ ACTION[shotState(clip)] }}</button>
      </div>
    </div>
  </div>
</template>
