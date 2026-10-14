<script setup>
// The playlist player widget (SPECIFICATION.md §8.7): pick a playlist (or `playlist`); Cue (its first clip, paused),
// Play / Pause, Stop (the idle playlist, §7) on the channel; the countdown to the playlist's end, the entry
// playing, and Loop (the playlist's own setting, saved at once).
import { computed, ref, watch } from "vue";
import { api, fmtNs } from "../api.js";
import { act, channels, channelState, clipsById, refreshLists, refreshStatus, state, transport } from "../store.js";

const props = defineProps({ channel: { type: Number, required: true }, playlist: { type: String, default: "" } });
const ch = computed(() => channels.value.find((c) => c.index === props.channel) || null);
const picked = ref(props.playlist);
watch(
  () => state.playlists.length,
  () => {
    // The playlist the channel is on, else the first.
    if (!picked.value && state.playlists.length) picked.value = state.playlists.find((p) => p.id === ch.value?.playlist)?.id || state.playlists[0].id;
  },
  { immediate: true },
);
const list = computed(() => state.playlists.find((p) => p.id === picked.value) || null);
// The channel is on this playlist (cued, playing, paused or ended).
const onThis = computed(() => !!ch.value && !!picked.value && ch.value.playlist === picked.value && !ch.value.live);
const shot = computed(() => (onThis.value ? ch.value.shot : "idle"));
const remaining = computed(() => (onThis.value ? ch.value.playlist_remaining_ns : null));
const soon = computed(() => remaining.value != null && ch.value?.playing && remaining.value < 5e9);
const current = computed(() => (onThis.value ? clipsById.value[ch.value.clip] : null));

const path = (action) => `/api/v1/playlists/${encodeURIComponent(picked.value)}/${action}`;
async function cue() {
  if (await act(() => api.post(path("cue"), { channel: props.channel }))) refreshStatus();
}
async function playPause() {
  if (onThis.value && ch.value.playing) return transport("pause", props.channel);
  if (onThis.value && (shot.value === "cued" || shot.value === "paused")) return transport("play", props.channel);
  if (await act(() => api.post(path("play"), { channel: props.channel }))) refreshStatus();
}
async function toggleLoop() {
  if (await act(() => api.put(`/api/v1/playlists/${encodeURIComponent(picked.value)}`, { loop: !list.value.loop }))) refreshLists();
}
</script>

<template>
  <div v-if="ch" class="widget-pl">
    <div class="widget-head">
      <span class="name">{{ ch.label }}</span>
      <span class="state-tag" :class="channelState(ch)">{{ channelState(ch).toUpperCase() }}</span>
      <select v-if="!playlist" v-model="picked" aria-label="Playlist">
        <option v-if="!state.playlists.length" value="">No playlists yet</option>
        <option v-for="p in state.playlists" :key="p.id" :value="p.id">{{ p.name }}</option>
      </select>
      <span v-else class="name">{{ list?.name || playlist }}</span>
      <span v-if="playlist" class="spacer"></span>
      <span class="tc">{{ ch.timecode || "--:--:--:--" }}</span>
    </div>
    <template v-if="list">
      <div class="widget-big">
        <span class="cd" :class="{ soon }" title="to the playlist's end at the speed set">{{ remaining == null ? "–" : `−${fmtNs(remaining)}` }}</span>
        <span class="muted small">
          <template v-if="onThis">entry {{ ch.playlist_index + 1 }} / {{ list.entries }}<template v-if="current"> · {{ current.name }}</template></template>
          <template v-else>{{ list.entries }} clips · {{ fmtNs(list.duration_ns) }}<template v-if="ch.playlist && !ch.live"> · {{ ch.label }} is on another playlist</template></template>
        </span>
        <span class="spacer"></span>
        <span v-if="ch.idle && onThis" class="state-tag idle" title="the channel's idle playlist">IDLE</span>
      </div>
      <div class="btn-grid four">
        <button class="btn secondary" :disabled="!list.entries" title="Cue the first clip, paused" :aria-pressed="shot === 'cued'" @click="cue">Cue</button>
        <button class="btn" :disabled="!list.entries" :aria-pressed="onThis && ch.playing" @click="playPause">{{ onThis && ch.playing ? "Pause" : "Play" }}</button>
        <button class="btn secondary" :title="ch.idle_playlist ? 'Stop: the idle playlist plays' : 'Stop: the shot ends where it stands'" @click="transport('stop', ch.index)">
          Stop
        </button>
        <button class="btn secondary" :aria-pressed="!!list.loop" title="Loop: after the last clip, start again at the first" @click="toggleLoop">
          Loop {{ list.loop ? "on" : "off" }}
        </button>
      </div>
    </template>
    <div v-else-if="picked" class="empty">No playlist {{ picked }}</div>
    <div v-else class="empty">No playlists yet: create one in the Playlists tab.</div>
  </div>
  <div v-else class="empty">No channel {{ channel }}</div>
</template>
