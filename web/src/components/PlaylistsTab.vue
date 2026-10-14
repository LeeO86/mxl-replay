<script setup>
// Playlists (SPECIFICATION.md §7): ordered clips, each with its own speed, end action and
// auto-advance, a transition into it (cut or mix, audio crossfade, own audio); an aux audio track per playlist
// (1.7.0); play on the selected channel.
import { computed, ref, watch } from "vue";
import ChannelPicker from "./ChannelPicker.vue";
import { api, AUX_DEFAULTS, END_ACTIONS, entryTransition, fmtNs, playNs } from "../api.js";
import { act, cameraLabel, channels, clipsById, refreshLists, refreshStatus, state } from "../store.js";

const selectedId = ref("");
// { name, loop, aux, entries: [{ clip_id, speed (percent), end, auto_advance, transition, transition_frames, audio_frames, own_audio }] }
const draft = ref(null);
const saved = ref("");
const message = ref("");
const newName = ref("");
const addClip = ref("");

const selected = computed(() => state.playlists.find((p) => p.id === selectedId.value) || null);
const dirty = computed(() => draft.value && JSON.stringify(draft.value) !== saved.value);
// Each entry plays forward at 1–200 % (0 % would never reach the clip's OUT).
const speedOk = (speed) => typeof speed === "number" && speed >= 1 && speed <= 200;
const framesOk = (n, low) => Number.isInteger(n) && n >= low && n <= 250;
const entriesOk = computed(() =>
  (draft.value?.entries || []).every(
    (e) => speedOk(e.speed) && (e.transition !== "mix" || framesOk(e.transition_frames, 1)) && (e.audio_frames === -1 || framesOk(e.audio_frames, 0)),
  ),
);
// The aux source picker: audio items first, then clips (their audio).
const auxSources = computed(() => [
  ...state.audio.map((a) => ({ id: a.id, label: `♪ ${a.name} · ${fmtNs(a.duration_ns)}` })),
  ...state.clips.map((c) => ({ id: c.id, label: `${c.name} · ${cameraLabel(c.camera)} · ${fmtNs(c.out_ns - c.in_ns)}` })),
]);
// The audio crossfade field: empty follows the video (-1).
const audioFramesText = (e) => (e.audio_frames < 0 ? "" : String(e.audio_frames));
function setAudioFrames(e, text) {
  const n = Number(text);
  e.audio_frames = text === "" ? -1 : Number.isFinite(n) ? Math.round(n) : e.audio_frames;
}
function setTransition(e, kind) {
  e.transition = kind;
  if (kind === "mix" && !e.transition_frames) e.transition_frames = 25;
  if (kind === "cut") e.transition_frames = 0;
}
const playingOn = (id) => channels.value.filter((c) => c.playlist === id && !c.live);
const idleOn = (id) => channels.value.filter((c) => c.idle_playlist === id);

async function load(id) {
  // Another playlist: unsaved edits are only dropped when the operator says so.
  if (id !== selectedId.value && dirty.value && !confirm(`Discard the unsaved changes to "${draft.value.name}"?`)) return;
  selectedId.value = id;
  message.value = "";
  const playlist = await act(() => api.get(`/api/v1/playlists/${encodeURIComponent(id)}`));
  if (!playlist) return;
  draft.value = {
    name: playlist.name,
    loop: !!playlist.loop,
    aux: { ...AUX_DEFAULTS, ...(playlist.aux || {}) },
    entries: playlist.entries.map((e) => ({ clip_id: e.clip_id, speed: Math.round(e.speed * 100), end: e.end, auto_advance: e.auto_advance, ...entryTransition(e) })),
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
  draft.value.entries.push({
    clip_id: addClip.value,
    speed: Math.round((clipsById.value[addClip.value]?.speed ?? 1) * 100),
    end: "next",
    auto_advance: true,
    ...entryTransition({}),
  });
  addClip.value = "";
}
// The clips at their speeds; a transition into an auto-advanced clip overlaps the two by its length (as the server).
const overlapFrames = (e) => Math.max(e.transition === "mix" ? e.transition_frames : 0, e.audio_frames >= 0 ? e.audio_frames : e.transition === "mix" ? e.transition_frames : 0);
const draftLength = computed(() =>
  (draft.value?.entries || []).reduce((sum, e, i, list) => {
    const clip = clipsById.value[e.clip_id];
    if (!clip) return sum;
    const length = playNs(clip, e.speed / 100);
    const before = list[i - 1];
    const overlap = i > 0 && before.end === "next" && before.auto_advance ? Math.min(length, overlapFrames(e) * (state.status?.frame_ns || 0)) : 0;
    return sum + length - overlap;
  }, 0),
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
  const body = {
    name: draft.value.name.trim(),
    loop: draft.value.loop,
    aux: draft.value.aux,
    entries: draft.value.entries.map((e) => ({ ...e, speed: e.speed / 100 })),
  };
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
        <span v-for="c in idleOn(p.id)" :key="`idle${c.index}`" class="pill" style="margin-left: 0.4rem" title="the channel's idle playlist">idle {{ c.label }}</span>
        <div class="meta">{{ p.entries }} clips · {{ fmtNs(p.duration_ns) }}<template v-if="p.loop"> · loops</template></div>
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
      <label style="display: flex; gap: 0.5rem; align-items: center; justify-content: flex-start; margin-top: 0.6rem; text-transform: none; letter-spacing: 0; font-size: 0.9rem">
        <input v-model="draft.loop" type="checkbox" style="width: auto; flex: none; margin: 0" /> <span>Loop: after the last clip, start again at the first</span>
      </label>
      <table style="margin-top: 0.8rem">
        <thead>
          <tr>
            <th class="num">#</th>
            <th></th>
            <th>Clip</th>
            <th class="num">Length</th>
            <th>Speed %</th>
            <th>At the end</th>
            <th title="when the end action is Next: play the next clip at once (else cue it)">Auto-advance</th>
            <th title="into this clip from the one before: cut, or a mix of N frames; the audio crossfades over its own frames (empty: as the video)">Transition in</th>
            <th title="the clip's own audio; off: only the aux track plays (it is ducked while own audio plays)">Own audio</th>
            <th></th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="(e, i) in draft.entries" :key="i" :class="{ dim: !clipsById[e.clip_id] }">
            <td class="num">{{ i + 1 }}</td>
            <td class="thumb-cell small-thumb">
              <img
                v-if="clipsById[e.clip_id]"
                :src="`/api/v1/clips/${encodeURIComponent(e.clip_id)}/thumbnail.jpg?in=${clipsById[e.clip_id].in_ns}`"
                alt=""
                loading="lazy"
                @error="$event.target.style.visibility = 'hidden'"
              />
              <div v-else class="noimg"></div>
            </td>
            <td>
              <template v-if="clipsById[e.clip_id]">
                <span class="swatch" :style="{ background: clipsById[e.clip_id].colour }"></span> {{ clipsById[e.clip_id].name }}
                <div class="muted small">{{ cameraLabel(clipsById[e.clip_id].camera) }} · {{ clipsById[e.clip_id].in_tc }}</div>
              </template>
              <span v-else class="muted">{{ e.clip_id }} (deleted, skipped)</span>
            </td>
            <td class="num">{{ clipsById[e.clip_id] ? fmtNs(playNs(clipsById[e.clip_id], e.speed / 100)) : "–" }}</td>
            <td><input v-model.number="e.speed" class="narrow" type="number" min="1" max="200" step="1" :class="{ invalid: !speedOk(e.speed) }" :aria-label="`Speed of entry ${i + 1}`" /></td>
            <td>
              <select v-model="e.end" class="end-select" :aria-label="`End action of entry ${i + 1}`">
                <option v-for="a in END_ACTIONS" :key="a.value" :value="a.value">{{ a.label }}</option>
              </select>
              <div v-if="draft.loop && i === draft.entries.length - 1 && (e.end === 'freeze' || e.end === 'black')" class="muted small">↻ Loop: wraps to the first clip</div>
            </td>
            <td><input v-model="e.auto_advance" type="checkbox" :aria-label="`Auto-advance after entry ${i + 1}`" /></td>
            <td class="transition-cell">
              <select :value="e.transition" :aria-label="`Transition into entry ${i + 1}`" @change="setTransition(e, $event.target.value)">
                <option value="cut">Cut</option>
                <option value="mix">Mix</option>
              </select>
              <input
                v-if="e.transition === 'mix'"
                v-model.number="e.transition_frames"
                type="number"
                min="1"
                max="250"
                :class="{ invalid: !framesOk(e.transition_frames, 1) }"
                :aria-label="`Mix frames of entry ${i + 1}`"
                title="mix frames"
              />
              <input
                class="audio-frames"
                :value="audioFramesText(e)"
                type="number"
                min="0"
                max="250"
                :placeholder="e.transition === 'mix' ? `♪ ${e.transition_frames}` : '♪ 0'"
                :class="{ invalid: e.audio_frames !== -1 && !framesOk(e.audio_frames, 0) }"
                :aria-label="`Audio crossfade frames of entry ${i + 1}`"
                title="audio crossfade frames (empty: as the video)"
                @input="setAudioFrames(e, $event.target.value)"
              />
              <div v-if="i === 0 && e.transition === 'mix' && !draft.loop" class="muted small">only on a loop wrap</div>
            </td>
            <td><input v-model="e.own_audio" type="checkbox" :aria-label="`Own audio of entry ${i + 1}`" /></td>
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
      <fieldset class="aux">
        <legend>Aux track <span class="muted small">audio under the playlist, ducked while a clip's own audio plays</span></legend>
        <div class="row">
          <div style="flex: 3">
            <label for="aux-source">Source</label>
            <select id="aux-source" v-model="draft.aux.source">
              <option value="">None</option>
              <option v-for="s in auxSources" :key="s.id" :value="s.id">{{ s.label }}</option>
            </select>
          </div>
          <div>
            <label for="aux-level">Level dB</label>
            <input id="aux-level" v-model.number="draft.aux.level_db" type="number" min="-60" max="12" step="1" :disabled="!draft.aux.source" />
          </div>
          <div>
            <label for="aux-fade">Fade-out frames</label>
            <input id="aux-fade" v-model.number="draft.aux.fade_out_frames" type="number" min="0" max="1000" :disabled="!draft.aux.source" />
          </div>
          <label class="check" style="flex: none; align-self: flex-end">
            <input v-model="draft.aux.loop" type="checkbox" :disabled="!draft.aux.source" /> <span>Loop</span>
          </label>
        </div>
        <div class="row">
          <div>
            <label for="aux-duck">Duck dB</label>
            <input id="aux-duck" v-model.number="draft.aux.duck_db" type="number" min="-60" max="0" step="1" :disabled="!draft.aux.source" />
          </div>
          <div>
            <label for="aux-attack">Attack ms</label>
            <input id="aux-attack" v-model.number="draft.aux.attack_ms" type="number" min="0" max="5000" step="10" :disabled="!draft.aux.source" />
          </div>
          <div>
            <label for="aux-release">Release ms</label>
            <input id="aux-release" v-model.number="draft.aux.release_ms" type="number" min="0" max="10000" step="10" :disabled="!draft.aux.source" />
          </div>
        </div>
      </fieldset>
      <div class="note">
        Total {{ fmtNs(draftLength) }}. <em>Mix</em> blends into a clip over its frames (the clips overlap by the longer of mix and audio crossfade; the next clip
        must follow with <em>Next</em> and auto-advance, else it cuts). The audio crossfades with equal power. At the end: <em>Next</em> goes to the next clip; with auto-advance it plays at once, otherwise it waits cued.
        With <em>Loop</em> on, the last clip's <em>Freeze</em> or <em>Black</em> wraps to the first clip; its own <em>Loop</em> or <em>Return to live</em> still apply.
        As a channel's idle playlist (Settings) every clip plays on and the playlist loops.
      </div>
      <div v-if="!entriesOk" class="msg err">Each speed is 1 to 200 %.</div>
      <div v-if="message" class="msg err">{{ message }}</div>
      <div class="actions">
        <button class="btn danger" @click="remove">Delete</button>
        <span class="spacer"></span>
        <button class="btn secondary" :disabled="!dirty" @click="load(selectedId)">Revert</button>
        <button class="btn" :disabled="!dirty || !entriesOk" @click="save">Save</button>
        <button class="btn" :disabled="dirty || !draft.entries.length" :title="dirty ? 'save first' : ''" @click="play(selectedId)">
          Play on {{ channels.find((c) => c.index === state.channel)?.label }}
        </button>
      </div>
    </div>
    <div v-if="!draft" class="panel empty">Create a playlist on the left, or pick one to edit its clips.</div>
  </div>
</template>
