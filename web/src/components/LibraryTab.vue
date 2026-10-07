<script setup>
// Library (SPECIFICATION.md §7, §8.3): every clip and upload, search, edit, export,
// consolidate, delete, and file upload.
import { computed, ref } from "vue";
import ClipInspector from "./ClipInspector.vue";
import Segmented from "./Segmented.vue";
import { api, END_ACTIONS, fmtNs, fmtSpeed } from "../api.js";
import { act, cameraLabel, refreshLists, state } from "../store.js";

const query = ref("");
const source = ref("all");
const sources = [
  { value: "all", label: "All" },
  { value: "buffer", label: "Buffer" },
  { value: "library", label: "Library" },
];
const editing = ref(null);

function sourceOf(clip) {
  if (clip.camera === 0) return { text: "Upload", kind: "neutral" };
  return clip.library ? { text: "Consolidated", kind: "ok" } : { text: "Buffer", kind: "warn" };
}
const tagsOf = (clip) =>
  clip.tags
    .split(",")
    .map((t) => t.trim())
    .filter(Boolean);
const endLabel = (value) => END_ACTIONS.find((e) => e.value === value)?.label || value;

const rows = computed(() => {
  const words = query.value.toLowerCase().split(/\s+/).filter(Boolean);
  // Newest first.
  return state.clips
    .filter((clip) => {
      if (source.value === "buffer" && clip.library) return false;
      if (source.value === "library" && !clip.library) return false;
      const text = `${clip.name} ${clip.id} ${clip.tags} ${cameraLabel(clip.camera)} ${clip.in_tc}`.toLowerCase();
      return words.every((w) => text.includes(w));
    })
    .reverse();
});

async function exportClip(clip) {
  const result = await act(() => api.post(`/api/v1/clips/${encodeURIComponent(clip.id)}/export`));
  if (result) state.notice = `Exported ${clip.name} to ${result.path} (JPEG frames and audio.wav).`;
}
async function consolidate(clip) {
  if (!confirm(`Consolidate "${clip.name}"? It is copied into library storage and its range in the buffer is released.`)) return;
  if (await act(() => api.post(`/api/v1/clips/${encodeURIComponent(clip.id)}/consolidate`))) {
    state.notice = `Consolidated ${clip.name}.`;
    refreshLists();
  }
}
async function remove(clip) {
  if (!confirm(`Delete "${clip.name}"? Its frames in the buffer are no longer kept.`)) return;
  if (await act(() => api.del(`/api/v1/clips/${encodeURIComponent(clip.id)}`))) refreshLists();
}

// Upload: the file is the request body, the name a query parameter. XMLHttpRequest for progress.
const uploadName = ref("");
const fileInput = ref(null);
const file = ref(null);
const progress = ref(-1); // −1 idle, 0–1 sending, 2 converting
const uploadMessage = ref("");
const uploadOk = ref(false);
function pick(event) {
  file.value = event.target.files?.[0] || null;
  if (file.value && !uploadName.value) uploadName.value = file.value.name.replace(/\.[^.]+$/, "");
}
function upload() {
  if (!file.value) return;
  uploadMessage.value = "";
  progress.value = 0;
  const xhr = new XMLHttpRequest();
  xhr.open("POST", `/api/v1/uploads?name=${encodeURIComponent(uploadName.value.trim())}`);
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) progress.value = e.loaded / e.total;
  };
  xhr.upload.onload = () => {
    progress.value = 2;
  };
  xhr.onload = () => {
    let body = {};
    try {
      body = JSON.parse(xhr.responseText);
    } catch {
      /* not JSON */
    }
    uploadOk.value = xhr.status === 201;
    uploadMessage.value = uploadOk.value ? `Uploaded as ${body.id}.` : `Upload failed: ${body.error || xhr.statusText}`;
    progress.value = -1;
    if (uploadOk.value) {
      file.value = null;
      uploadName.value = "";
      if (fileInput.value) fileInput.value.value = "";
      refreshLists();
    }
  };
  xhr.onerror = () => {
    uploadOk.value = false;
    uploadMessage.value = "Upload failed: the connection was lost.";
    progress.value = -1;
  };
  xhr.send(file.value);
}
</script>

<template>
  <div class="panel">
    <h3>Upload a file</h3>
    <div class="row">
      <div style="flex: 2">
        <label for="up-file">Video or still image (converted to the house format)</label>
        <input id="up-file" ref="fileInput" type="file" accept="video/*,image/*,.mxf,.mov,.mkv" :disabled="progress >= 0" @change="pick" />
      </div>
      <div style="flex: 2">
        <label for="up-name">Name</label>
        <input id="up-name" v-model="uploadName" placeholder="automatic: upload-<number>" :disabled="progress >= 0" />
      </div>
      <button class="btn" style="flex: none" :disabled="!file || progress >= 0" @click="upload">Upload</button>
    </div>
    <div v-if="progress >= 0 && progress <= 1" class="progressbar"><div :style="{ width: `${progress * 100}%` }"></div></div>
    <div v-if="progress >= 0" class="msg">{{ progress > 1 ? "Converting… (the replay converts the whole file before it answers)" : `Sending ${Math.round(progress * 100)}%` }}</div>
    <div v-if="uploadMessage" class="msg" :class="uploadOk ? 'ok' : 'err'">{{ uploadMessage }}</div>
  </div>

  <div class="panel">
    <h3>
      Clips <span class="muted">{{ rows.length }} of {{ state.clips.length }}</span>
    </h3>
    <div class="row" style="align-items: center; margin-bottom: 0.6rem">
      <input v-model="query" type="search" placeholder="Search name, tag, camera or timecode" aria-label="Search clips" style="flex: 3" />
      <Segmented v-model="source" :options="sources" label="Source" style="flex: none" />
    </div>
    <table v-if="rows.length">
      <thead>
        <tr>
          <th></th>
          <th>Name</th>
          <th>Camera</th>
          <th>IN</th>
          <th class="num">Length</th>
          <th class="num">Speed</th>
          <th>Motion / audio</th>
          <th>End</th>
          <th>Tags</th>
          <th>Source</th>
          <th></th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="clip in rows" :key="clip.id">
          <td class="thumb-cell"><img :src="`/api/v1/clips/${encodeURIComponent(clip.id)}/thumbnail.jpg?in=${clip.in_ns}`" alt="" loading="lazy" /></td>
          <td>
            <span class="swatch" :style="{ background: clip.colour }"></span> <strong>{{ clip.name }}</strong>
            <div class="muted small">{{ clip.id }}<span v-if="clip.group"> · {{ clip.group }}</span></div>
          </td>
          <td>{{ cameraLabel(clip.camera) }}</td>
          <td class="num" style="font-family: ui-monospace, monospace">{{ clip.in_tc }}</td>
          <td class="num">{{ fmtNs(clip.out_ns - clip.in_ns) }}</td>
          <td class="num">{{ fmtSpeed(clip.speed) }}</td>
          <td>{{ clip.motion }} / {{ clip.audio }}</td>
          <td>{{ endLabel(clip.end) }}</td>
          <td>
            <div class="tags"><span v-for="t in tagsOf(clip)" :key="t" class="tag">{{ t }}</span></div>
          </td>
          <td><span class="pill" :class="sourceOf(clip).kind">{{ sourceOf(clip).text }}</span></td>
          <td class="actions-cell">
            <button class="btn small" @click="editing = clip">Edit</button>
            <button class="btn small secondary" @click="exportClip(clip)">Export</button>
            <button class="btn small secondary" :disabled="clip.library" :title="clip.library ? 'already in library storage' : ''" @click="consolidate(clip)">
              Consolidate
            </button>
            <button class="btn small danger" @click="remove(clip)">Delete</button>
          </td>
        </tr>
      </tbody>
    </table>
    <div v-else class="empty">{{ state.clips.length ? "No clip matches the search." : "No clips yet: create one on the LSM page or upload a file." }}</div>
  </div>

  <ClipInspector :clip="editing" @close="editing = null" />
</template>
