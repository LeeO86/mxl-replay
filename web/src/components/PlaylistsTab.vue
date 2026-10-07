<script setup>
// Playlists (SPECIFICATION.md §7): ordered clips, each with its own speed, end action and
// auto-advance; play on the selected channel.
import { computed, ref, watch } from "vue";
import ChannelPicker from "./ChannelPicker.vue";
import { api, END_ACTIONS, fmtNs, playNs } from "../api.js";
import { act, cameraLabel, channels, clipsById, refreshLists, refreshStatus, state } from "../store.js";

const selectedId = ref("");
const draft = ref(null); // { name, entries: [{ clip_id, speed (percent), end, auto_advance }] }
const saved = ref("");
const message = ref("");
const newName = ref("");
const addClip = ref("");

const selected = computed(() => state.playlists.find((p) => p.id === selectedId.value) || null);
const dirty = computed(() => draft.value && JSON.stringify(draft.value) !== saved.value);
const playingOn = (id) => channels.value.filter((c) => c.playlist === id && !c.live);

async function load(id) {
  selectedId.value = id;
  message.value = "";
  const playlist = await act(() => api.get(`/api/v1/playlists/${encodeURIComponent(id)}`));
  if (!playlist) return;
  draft.value = {
    name: playlist.name,
    entries: playlist.entries.map((e) => ({ clip_id: e.clip_id, speed: Math.round(e.speed * 100), end: e.end, auto_advance: e.auto_advance })),
  };
  saved.value = JSON.stringify(draft.value);
}
watch(
  () => state.playlists.length,
  () => {
    if (!selectedId.value && state.playlists.length) load(state.playlists[0].id);
  },
  { immediate: true },
);

function move(index, by) {
  const list = draft.value.entries;
  const [item] = list.splice(index, 1);
  list.splice(index + by, 0, item);
}
function add() {
  if (!addClip.value) return;
  draft.value.entries.push({ clip_id: addClip.value, speed: Math.round((clipsById.value[addClip.value]?.speed ?? 1) * 100), end: "next", auto_advance: true });
  addClip.value = "";
}
const draftLength = computed(() =>
  (draft.value?.entries || []).reduce((sum, e) => sum + (clipsById.value[e.clip_id] ? playNs(clipsById.value[e.clip_id], e.speed / 100) : 0), 0),
);

async function create() {
  const created = await act(() => api.post("/api/v1/playlists", { name: newName.value.trim(), clips: [] }));
  if (!created) return;
  newName.value = "";
  await refreshLists();
  load(created.id);
}
async function save() {
  message.value = "";
  const body = { name: draft.value.name.trim(), entries: draft.value.entries.map((e) => ({ ...e, speed: e.speed / 100 })) };
  try {
    await api.put(`/api/v1/playlists/${encodeURIComponent(selectedId.value)}`, body);
    saved.value = JSON.stringify(draft.value);
    state.notice = `Saved playlist ${body.name || selectedId.value}.`;
    refreshLists();
  } catch (e) {
    message.value = e.message;
  }
}
async function remove() {
  if (!confirm(`Delete playlist "${selected.value?.name}"? The clips stay.`)) return;
  if (await act(() => api.del(`/api/v1/playlists/${encodeURIComponent(selectedId.value)}`))) {
    selectedId.value = "";
    draft.value = null;
    await refreshLists();
    if (state.playlists.length) load(state.playlists[0].id);
  }
}
async function play(id) {
  if (await act(() => api.post(`/api/v1/playlists/${encodeURIComponent(id)}/play`, { channel: state.channel }))) refreshStatus();
}
</script>

<template>
  <div class="toolbar">
    <ChannelPicker />
    <span class="muted small">Play sends the playlist to this channel.</span>
  </div>
  <div class="split">
    <div class="panel">
      <h3>Playlists</h3>
      <button
        v-for="p in state.playlists"
        :key="p.id"
        class="listbtn"
        :class="{ active: p.id === selectedId }"
        :aria-pressed="p.id === selectedId"
        @click="load(p.id)"
      >
        <strong>{{ p.name }}</strong>
        <span v-for="c in playingOn(p.id)" :key="c.index" class="pill ok" style="margin-left: 0.4rem">{{ c.label }} {{ c.playing ? "playing" : c.shot }}</span>
        <div class="meta">{{ p.entries }} clips · {{ fmtNs(p.duration_ns) }}</div>
      </button>
      <div v-if="!state.playlists.length" class="empty">No playlists yet.</div>
      <form class="row" style="margin-top: 0.8rem" @submit.prevent="create">
        <input v-model="newName" placeholder="New playlist name" aria-label="New playlist name" style="flex: 2" />
        <button class="btn" style="flex: none" type="submit">Create</button>
      </form>
    </div>

    <div v-if="draft" class="panel">
      <h3>
        Edit playlist <span class="muted">{{ selectedId }}</span>
        <span class="spacer"></span>
        <span v-for="c in playingOn(selectedId)" :key="c.index" class="pill ok">
          {{ c.label }}: {{ c.playing ? "playing" : c.shot }} entry {{ c.playlist_index + 1 }}
        </span>
      </h3>
      <label for="pl-name">Name</label>
      <input id="pl-name" v-model="draft.name" maxlength="120" />
      <table style="margin-top: 0.8rem">
        <thead>
          <tr>
            <th class="num">#</th>
            <th>Clip</th>
            <th class="num">Length</th>
            <th>Speed %</th>
            <th>At the end</th>
            <th title="when the end action is Next: play the next clip at once (else cue it)">Auto-advance</th>
            <th></th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="(e, i) in draft.entries" :key="i" :class="{ dim: !clipsById[e.clip_id] }">
            <td class="num">{{ i + 1 }}</td>
            <td>
              <template v-if="clipsById[e.clip_id]">
                <span class="swatch" :style="{ background: clipsById[e.clip_id].colour }"></span> {{ clipsById[e.clip_id].name }}
                <div class="muted small">{{ cameraLabel(clipsById[e.clip_id].camera) }} · {{ clipsById[e.clip_id].in_tc }}</div>
              </template>
              <span v-else class="muted">{{ e.clip_id }} (deleted, skipped)</span>
            </td>
            <td class="num">{{ clipsById[e.clip_id] ? fmtNs(playNs(clipsById[e.clip_id], e.speed / 100)) : "–" }}</td>
            <td><input v-model.number="e.speed" type="number" min="0" max="200" step="1" :aria-label="`Speed of entry ${i + 1}`" /></td>
            <td>
              <select v-model="e.end" :aria-label="`End action of entry ${i + 1}`">
                <option v-for="a in END_ACTIONS" :key="a.value" :value="a.value">{{ a.label }}</option>
              </select>
            </td>
            <td><input v-model="e.auto_advance" type="checkbox" :aria-label="`Auto-advance after entry ${i + 1}`" /></td>
            <td class="actions-cell">
              <button class="btn small secondary" :disabled="i === 0" title="Move up" @click="move(i, -1)">↑</button>
              <button class="btn small secondary" :disabled="i === draft.entries.length - 1" title="Move down" @click="move(i, 1)">↓</button>
              <button class="btn small danger" title="Remove from the playlist" @click="draft.entries.splice(i, 1)">Remove</button>
            </td>
          </tr>
        </tbody>
      </table>
      <div v-if="!draft.entries.length" class="empty">No clips in this playlist yet: add some below.</div>
      <div class="row" style="margin-top: 0.7rem">
        <select v-model="addClip" aria-label="Clip to add" style="flex: 3">
          <option value="">Add a clip…</option>
          <option v-for="c in state.clips" :key="c.id" :value="c.id">{{ c.name }} · {{ cameraLabel(c.camera) }} · {{ fmtNs(c.out_ns - c.in_ns) }}</option>
        </select>
        <button class="btn secondary" style="flex: none" :disabled="!addClip" @click="add">Add</button>
      </div>
      <div class="note">Total {{ fmtNs(draftLength) }}. At the end: <em>Next</em> goes to the next clip; with auto-advance it plays at once, otherwise it waits cued.</div>
      <div v-if="message" class="msg err">{{ message }}</div>
      <div class="actions">
        <button class="btn danger" @click="remove">Delete</button>
        <span class="spacer"></span>
        <button class="btn secondary" :disabled="!dirty" @click="load(selectedId)">Revert</button>
        <button class="btn" :disabled="!dirty" @click="save">Save</button>
        <button class="btn" :disabled="dirty || !draft.entries.length" :title="dirty ? 'save first' : ''" @click="play(selectedId)">
          Play on {{ channels.find((c) => c.index === state.channel)?.label }}
        </button>
      </div>
    </div>
    <div v-if="!draft" class="panel empty">Create a playlist on the left, or pick one to edit its clips.</div>
  </div>
</template>
