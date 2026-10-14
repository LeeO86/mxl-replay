<script setup>
// The playlist editor widget (SPECIFICATION.md §8.7): one playlist's entries. Add a clip, reorder, end action,
// speed, remove; every change is saved at once (no Save button). Without `playlist` the widget has a playlist picker.
import { computed, ref, watch } from "vue";
import { api, END_ACTIONS, fmtNs, playNs } from "../api.js";
import { act, cameraLabel, channels, clipsById, refreshLists, state } from "../store.js";

const props = defineProps({ playlist: { type: String, default: "" } });
const picked = ref(props.playlist);
const entries = ref([]); // [{ clip_id, speed (percent), end, auto_advance }]
const loaded = ref(false);
const missing = ref(false);
const addClip = ref("");
const saving = ref(false);

const summary = computed(() => state.playlists.find((p) => p.id === picked.value) || null);
const playingOn = computed(() => channels.value.filter((c) => c.playlist === picked.value && !c.live));
const total = computed(() => entries.value.reduce((sum, e) => sum + (clipsById.value[e.clip_id] ? playNs(clipsById.value[e.clip_id], e.speed / 100) : 0), 0));
const speedOk = (speed) => typeof speed === "number" && speed >= 1 && speed <= 200;

async function load() {
  if (!picked.value) return;
  try {
    const playlist = await api.get(`/api/v1/playlists/${encodeURIComponent(picked.value)}`);
    entries.value = playlist.entries.map((e) => ({ clip_id: e.clip_id, speed: Math.round(e.speed * 100), end: e.end, auto_advance: e.auto_advance }));
    missing.value = false;
  } catch (e) {
    missing.value = e.status === 404;
    entries.value = [];
    if (!missing.value) state.error = e.message;
  }
  loaded.value = true;
}
// The first playlist when none is given; reload when the playlist changes elsewhere (the list refreshes every 5 s).
watch(
  () => state.playlists.length,
  () => {
    if (!picked.value && !props.playlist && state.playlists.length) picked.value = state.playlists[0].id;
  },
  { immediate: true },
);
watch(picked, () => {
  loaded.value = false;
  load();
}, { immediate: true });
watch(
  () => (summary.value ? `${summary.value.entries} ${summary.value.duration_ns} ${(summary.value.clip_ids || []).join(",")}` : ""),
  (now, before) => {
    if (before !== undefined && !saving.value) load();
  },
);

async function save() {
  if (!entries.value.every((e) => speedOk(e.speed))) {
    state.error = "Each speed is 1 to 200 %.";
    return;
  }
  saving.value = true;
  const body = { entries: entries.value.map((e) => ({ ...e, speed: e.speed / 100 })) };
  if (await act(() => api.put(`/api/v1/playlists/${encodeURIComponent(picked.value)}`, body))) await refreshLists();
  saving.value = false;
}
function move(index, by) {
  const [item] = entries.value.splice(index, 1);
  entries.value.splice(index + by, 0, item);
  save();
}
function remove(index) {
  entries.value.splice(index, 1);
  save();
}
function add() {
  if (!addClip.value) return;
  entries.value.push({ clip_id: addClip.value, speed: Math.round((clipsById.value[addClip.value]?.speed ?? 1) * 100), end: "next", auto_advance: true });
  addClip.value = "";
  save();
}
</script>

<template>
  <div class="widget-pl">
    <div class="widget-head">
      <select v-if="!playlist" v-model="picked" aria-label="Playlist">
        <option v-if="!state.playlists.length" value="">No playlists yet</option>
        <option v-for="p in state.playlists" :key="p.id" :value="p.id">{{ p.name }}</option>
      </select>
      <span v-else class="name">{{ summary?.name || playlist }}</span>
      <span v-for="c in playingOn" :key="c.index" class="pill ok">{{ c.label }} entry {{ c.playlist_index + 1 }}</span>
      <span class="spacer"></span>
      <span class="muted small">{{ entries.length }} clips · <span class="num">{{ fmtNs(total) }}</span><template v-if="summary?.loop"> · loops</template></span>
    </div>
    <div v-if="missing" class="empty">No playlist {{ picked }}</div>
    <template v-else-if="picked && loaded">
      <div class="widget-entries">
        <div v-if="!entries.length" class="empty">No clips in this playlist yet: add one below.</div>
        <div
          v-for="(e, i) in entries"
          :key="`${i}-${e.clip_id}`"
          class="widget-entry"
          :class="{ dim: !clipsById[e.clip_id], current: playingOn.some((c) => c.playlist_index === i) }"
        >
          <span class="stripe" :style="{ background: clipsById[e.clip_id]?.colour || 'transparent' }"></span>
          <span class="idx num">{{ i + 1 }}</span>
          <div class="text">
            <div class="name" :title="e.clip_id">{{ clipsById[e.clip_id]?.name || `${e.clip_id} (deleted, skipped)` }}</div>
            <div v-if="clipsById[e.clip_id]" class="meta">
              {{ cameraLabel(clipsById[e.clip_id].camera) }} · <span class="num">{{ fmtNs(playNs(clipsById[e.clip_id], e.speed / 100)) }}</span>
            </div>
          </div>
          <input
            v-model.number="e.speed"
            type="number"
            min="1"
            max="200"
            step="1"
            :class="{ invalid: !speedOk(e.speed) }"
            :aria-label="`Speed % of entry ${i + 1}`"
            title="Speed %"
            @change="save"
          />
          <select v-model="e.end" :aria-label="`End action of entry ${i + 1}`" title="At the end" @change="save">
            <option v-for="a in END_ACTIONS" :key="a.value" :value="a.value">{{ a.label }}</option>
          </select>
          <button class="btn small secondary" :disabled="i === 0" title="Move up" @click="move(i, -1)">↑</button>
          <button class="btn small secondary" :disabled="i === entries.length - 1" title="Move down" @click="move(i, 1)">↓</button>
          <button class="btn small danger" title="Remove from the playlist" @click="remove(i)">✕</button>
          <span v-if="summary?.loop && i === entries.length - 1 && (e.end === 'freeze' || e.end === 'black')" class="muted small" title="Loop is on: the playlist wraps here">↻ wraps</span>
        </div>
      </div>
      <div class="widget-row">
        <select v-model="addClip" aria-label="Clip to add">
          <option value="">Add a clip…</option>
          <option v-for="c in [...state.clips].reverse()" :key="c.id" :value="c.id">{{ c.name }} · {{ cameraLabel(c.camera) }} · {{ fmtNs(c.out_ns - c.in_ns) }}</option>
        </select>
        <button class="btn small" :disabled="!addClip" @click="add">Add</button>
      </div>
    </template>
    <div v-else-if="!picked" class="empty">No playlists yet: create one in the Playlists tab.</div>
  </div>
</template>
