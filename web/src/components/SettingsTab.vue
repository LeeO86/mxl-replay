<script setup>
// Settings: configuration export/import as one JSON document (SPECIFICATION.md §8.5), the
// keyboard map and the jog/shuttle controller (§8.4).
import { computed, onMounted, onUnmounted, ref } from "vue";
import { api } from "../api.js";
import { ACTIONS, comboOf, keyLabel, keymap, resetKeys, setKey } from "../keys.js";
import { BUTTON_LAYOUTS, connectHid, disconnectHid, hid, SHUTTLE_SPEEDS } from "../hid.js";
import { act, refreshLists, refreshStatus, state } from "../store.js";

const exported = ref(null);
const exportText = computed(() => (exported.value ? JSON.stringify(exported.value, null, 2) : ""));
const copied = ref("");
async function loadExport() {
  const doc = await act(() => api.get("/api/v1/config/export"));
  if (doc) exported.value = doc;
}
function download() {
  const blob = new Blob([exportText.value + "\n"], { type: "application/json" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = `mxl-replay-${exported.value?.settings?.NMOS_SEED || "config"}.json`;
  a.click();
  URL.revokeObjectURL(a.href);
}
async function copy() {
  try {
    await navigator.clipboard.writeText(exportText.value);
    copied.value = "Copied.";
  } catch {
    copied.value = "The browser does not allow copying here: select the text and copy it.";
  }
}

const importText = ref("");
const importMessage = ref("");
const importOk = ref(false);
function onFile(event) {
  event.target.files?.[0]?.text().then((t) => (importText.value = t));
}
async function doImport() {
  importMessage.value = "";
  try {
    JSON.parse(importText.value);
  } catch (e) {
    importOk.value = false;
    importMessage.value = `Not JSON: ${e.message}`;
    return;
  }
  try {
    const result = await api.post("/api/v1/config/import", importText.value);
    importOk.value = true;
    importMessage.value = result.restart_required
      ? "Imported. Some settings need a restart of the replay (ports, format, counts, seed or domain)."
      : "Imported. Labels and modes apply now; clips and playlists in the document were added.";
    state.restartRequired = state.restartRequired || result.restart_required;
    loadExport();
    refreshLists();
    refreshStatus();
  } catch (e) {
    importOk.value = false;
    importMessage.value = e.message;
  }
}

// Keyboard map: "Change" listens for the next key.
const capturing = ref("");
function onCapture(event) {
  if (!capturing.value) return;
  event.preventDefault();
  event.stopPropagation();
  if (event.key === "Escape") {
    capturing.value = "";
    return;
  }
  const combo = comboOf(event);
  if (!combo) return;
  setKey(capturing.value, combo);
  capturing.value = "";
}
onMounted(() => {
  loadExport();
  window.addEventListener("keydown", onCapture, true);
});
onUnmounted(() => window.removeEventListener("keydown", onCapture, true));

const ACTION_LABELS = { ...Object.fromEntries(ACTIONS.map((a) => [a.id, a.label])), camera1: "Camera 1", camera2: "Camera 2", camera3: "Camera 3", camera4: "Camera 4", speed50: "Speed 50 %", speed100: "Speed 100 %" };
</script>

<template>
  <div class="grid two">
    <div class="panel">
      <h3>Export</h3>
      <p class="note">One JSON document: settings, clips and playlists. There are no secret settings.</p>
      <pre v-if="exported" style="max-height: 300px">{{ exportText }}</pre>
      <div class="actions">
        <span class="msg ok">{{ copied }}</span>
        <button class="btn secondary" @click="loadExport">Reload</button>
        <button class="btn secondary" :disabled="!exported" @click="copy">Copy</button>
        <button class="btn" :disabled="!exported" @click="download">Download</button>
      </div>
    </div>
    <div class="panel">
      <h3>Import</h3>
      <p class="note">An exported document, or <code>{"settings": {…}}</code> alone. Labels and modes apply at once; ports, format, counts, seed and domain after a restart.</p>
      <input type="file" accept="application/json,.json" aria-label="Import file" @change="onFile" />
      <textarea v-model="importText" placeholder="paste an exported document" aria-label="Document to import" style="margin-top: 0.5rem; min-height: 180px"></textarea>
      <div v-if="importMessage" class="msg" :class="importOk ? 'ok' : 'err'">{{ importMessage }}</div>
      <div class="actions">
        <button class="btn" :disabled="!importText.trim()" @click="doImport">Import</button>
      </div>
    </div>
  </div>

  <div v-if="exported" class="panel">
    <h3>Current settings</h3>
    <table>
      <thead>
        <tr><th>Key</th><th>Value</th></tr>
      </thead>
      <tbody>
        <tr v-for="(value, key) in exported.settings" :key="key">
          <td><code>{{ key }}</code></td>
          <td style="word-break: break-all">{{ value }}</td>
        </tr>
      </tbody>
    </table>
  </div>

  <div class="grid two">
    <div class="panel">
      <h3>Keyboard <span class="muted small">(Shotbox and LSM; kept in this browser)</span></h3>
      <table>
        <tbody>
          <tr v-for="a in ACTIONS" :key="a.id">
            <td>{{ a.label }}</td>
            <td>
              <span v-if="capturing === a.id" class="capture">press a key… (Esc cancels)</span>
              <kbd v-else-if="keymap[a.id]">{{ keyLabel(keymap[a.id]) }}</kbd>
              <span v-else class="muted">none</span>
            </td>
            <td style="text-align: right"><button class="btn small secondary" @click="capturing = a.id">Change</button></td>
          </tr>
          <tr>
            <td>Select button (Shotbox) / camera (LSM)</td>
            <td><kbd>1</kbd>–<kbd>9</kbd>, <kbd>0</kbd></td>
            <td style="text-align: right" class="muted small">fixed</td>
          </tr>
        </tbody>
      </table>
      <div class="actions"><button class="btn secondary" @click="resetKeys">Reset to defaults</button></div>
    </div>

    <div class="panel">
      <h3>Jog / shuttle (WebHID)</h3>
      <p class="note">Contour ShuttleXpress, ShuttlePRO and ShuttlePRO v2. The ring sets the speed and plays (centre pauses), the jog wheel steps frames on the selected channel.</p>
      <div v-if="!hid.supported" class="warnbox">This browser has no WebHID: use Chrome or Edge.</div>
      <div v-else-if="!hid.secure" class="warnbox">WebHID needs a secure page: open the UI over https or on localhost (for example through an SSH tunnel).</div>
      <template v-else>
        <dl class="kv">
          <dt>Device</dt>
          <dd>{{ hid.device || "none" }}</dd>
          <template v-if="hid.device">
            <dt>Ring</dt>
            <dd>{{ hid.shuttle }} ({{ Math.round(Math.sign(hid.shuttle) * SHUTTLE_SPEEDS[Math.abs(hid.shuttle)] * 100) }} %)</dd>
            <dt>Jog</dt>
            <dd>{{ hid.jog ?? "–" }}</dd>
            <dt>Buttons</dt>
            <dd>{{ hid.buttons.toString(2).padStart(16, "0") }}</dd>
          </template>
        </dl>
        <div v-if="hid.error" class="msg err">{{ hid.error }}</div>
        <div class="actions">
          <button v-if="hid.device" class="btn secondary" @click="disconnectHid">Disconnect</button>
          <button class="btn" @click="connectHid">{{ hid.device ? "Choose another device" : "Connect a device" }}</button>
        </div>
      </template>
      <table style="margin-top: 0.6rem">
        <thead>
          <tr><th>Controller</th><th>Buttons (left to right, top to bottom)</th></tr>
        </thead>
        <tbody>
          <tr v-for="(layout, name) in BUTTON_LAYOUTS" :key="name">
            <td class="nowrap">{{ name }}</td>
            <td>{{ Object.values(layout).map((a) => ACTION_LABELS[a] || a).join(" · ") }}</td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>
