<script setup>
// The playlist editor widget (SPECIFICATION.md §8.7): one playlist's entries. Add a clip, reorder, end action,
// speed, transition (cut or mix), own audio, remove, and the aux track's source and level (1.7.0); every change is
// saved at once (no Save button). Without `playlist` the widget has a playlist picker.
import { computed, ref, watch } from "vue";
import { api, END_ACTIONS, entryTransition, fmtNs, playNs, TRANSITION_PRESETS, transitionPreset } from "../api.js";
import { act, cameraLabel, channels, clipsById, refreshLists, state } from "../store.js";

const props = defineProps({ playlist: { type: String, default: "" } });
const picked = ref(props.playlist);
const entries = ref([]); // [{ clip_id, speed (percent), end, auto_advance, transition, transition_frames, audio_frames, own_audio }]
const aux = ref({ source: "", level_db: 0 });
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
    entries.value = playlist.entries.map((e) => ({ clip_id: e.clip_id, speed: Math.round(e.speed * 100), end: e.end, auto_advance: e.auto_advance, ...entryTransition(e) }));
    aux.value = { source: playlist.aux?.source || "", level_db: playlist.aux?.level_db ?? 0 };
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
  () => (summary.value ? `${summary.value.entries} ${summary.value.duration_ns} ${(summary.value.clip_ids || []).join(",")} ${summary.value.aux_source}` : ""),
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
  // The aux object is partial: duck, fade and loop stay as set on the Playlists page.
  const body = { entries: entries.value.map((e) => ({ ...e, speed: e.speed / 100 })), aux: { source: aux.value.source, level_db: aux.value.level_db } };
  if (await act(() => api.put(`/api/v1/playlists/${encodeURIComponent(picked.value)}`, body))) await refreshLists();
  saving.value = false;
}
// The transition presets, and a mix of another length (set on the Playlists page) as its own choice.
const presetsOf = (e) => {
  const value = transitionPreset(e);
  return TRANSITION_PRESETS.some((p) => p.value === value) ? TRANSITION_PRESETS : [...TRANSITION_PRESETS, { value, label: `Mix ${e.transition_frames}` }];
};
function setPreset(e, value) {
  const preset = TRANSITION_PRESETS.find((p) => p.value === value);
  if (!preset) return;
  e.transition = preset.transition;
  e.transition_frames = preset.frames;
  save();
}
function toggleOwn(e) {
  e.own_audio = !e.own_audio;
  save();
}
const auxSources = computed(() => [
  ...state.audio.map((a) => ({ id: a.id, label: `♪ ${a.name}` })),
  ...state.clips.map((c) => ({ id: c.id, label: c.name })),
]);

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
  entries.value.push({
    clip_id: addClip.value,
    speed: Math.round((clipsById.value[addClip.value]?.speed ?? 1) * 100),
    end: "next",
    auto_advance: true,
    ...entryTransition({}),
  });
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
          <img
            v-if="clipsById[e.clip_id]"
            class="wthumb"
            :src="`/api/v1/clips/${encodeURIComponent(e.clip_id)}/thumbnail.jpg?in=${clipsById[e.clip_id].in_ns}`"
            alt=""
            loading="lazy"
            @error="$event.target.style.visibility = 'hidden'"
          />
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
          <select
            :value="transitionPreset(e)"
            :aria-label="`Transition into entry ${i + 1}`"
            title="Transition into this clip (the audio crossfades as long, unless set on the Playlists page)"
            @change="setPreset(e, $event.target.value)"
          >
            <option v-for="p in presetsOf(e)" :key="p.value" :value="p.value">{{ p.label }}</option>
          </select>
          <button
            class="btn small secondary own"
            :class="{ off: !e.own_audio }"
            :aria-pressed="e.own_audio"
            :aria-label="`Own audio of entry ${i + 1}`"
            :title="e.own_audio ? 'Own audio on (the aux track ducks)' : 'Own audio off (only the aux track)'"
            @click="toggleOwn(e)"
          >
            ♪
          </button>
          <button class="btn small secondary" :disabled="i === 0" title="Move up" @click="move(i, -1)">↑</button>
          <button class="btn small secondary" :disabled="i === entries.length - 1" title="Move down" @click="move(i, 1)">↓</button>
          <button class="btn small danger" title="Remove from the playlist" @click="remove(i)">✕</button>
          <span v-if="summary?.loop && i === entries.length - 1 && (e.end === 'freeze' || e.end === 'black')" class="muted small" title="Loop is on: the playlist wraps here">↻ wraps</span>
        </div>
      </div>
      <div class="widget-row">
        <span class="muted small">Aux</span>
        <select v-model="aux.source" aria-label="Aux track source" @change="save">
          <option value="">None</option>
          <option v-for="s in auxSources" :key="s.id" :value="s.id">{{ s.label }}</option>
        </select>
        <input v-model.number="aux.level_db" type="number" min="-60" max="12" step="1" style="width: 4rem" aria-label="Aux level dB" title="Aux level dB" :disabled="!aux.source" @change="save" />
        <span class="muted small">dB</span>
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
