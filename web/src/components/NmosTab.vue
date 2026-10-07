<script setup>
// NMOS node, receivers (camera inputs and their routes) and senders (channel outputs).
import { onMounted, onUnmounted, ref } from "vue";
import Pill from "./Pill.vue";
import { api } from "../api.js";
import { act } from "../store.js";

const nmos = ref(null);
let timer = null;
async function load() {
  const result = await act(() => api.get("/api/v1/nmos"));
  if (result) nmos.value = result;
}
onMounted(() => {
  load();
  timer = setInterval(load, 3000);
});
onUnmounted(() => clearInterval(timer));
const stateKind = (s) => ({ running: "ok", waiting: "warn", unsupported: "bad" })[s] || "neutral";
</script>

<template>
  <div v-if="nmos">
    <div class="panel">
      <h3>Node</h3>
      <dl class="kv">
        <dt>Node label</dt>
        <dd>{{ nmos.node_label }}</dd>
        <dt>Node id</dt>
        <dd><code>{{ nmos.node_id }}</code></dd>
        <dt>Device label</dt>
        <dd>{{ nmos.device_label }}</dd>
        <dt>Device id</dt>
        <dd><code>{{ nmos.device_id }}</code></dd>
        <dt>Host address</dt>
        <dd>{{ nmos.host_address }}:{{ nmos.nmos_port }}</dd>
        <dt>Registry</dt>
        <dd>{{ nmos.registry || "none (not registered)" }}</dd>
        <dt>Output domain</dt>
        <dd><code>{{ nmos.domain_id }}</code></dd>
      </dl>
    </div>
    <div class="panel">
      <h3>Receivers (camera inputs)</h3>
      <table>
        <thead>
          <tr>
            <th>Camera</th>
            <th>Essence</th>
            <th>Receiver id</th>
            <th>State</th>
            <th>Sender</th>
            <th>Flow</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="r in nmos.receivers" :key="r.id">
            <td>{{ r.label }}</td>
            <td>{{ r.kind }}<span v-if="r.kind === 'video' && r.phase > 1"> phase {{ r.phase }}</span></td>
            <td><code>{{ r.id }}</code></td>
            <td><Pill :text="r.active ? r.state : 'not connected'" :kind="r.active ? stateKind(r.state) : 'neutral'" /></td>
            <td><code v-if="r.sender_id">{{ r.sender_id }}</code><span v-else class="muted">–</span></td>
            <td><code v-if="r.flow_id">{{ r.flow_id }}</code><span v-else class="muted">–</span></td>
          </tr>
        </tbody>
      </table>
    </div>
    <div class="panel">
      <h3>Senders (channel outputs)</h3>
      <table>
        <thead>
          <tr>
            <th>Channel</th>
            <th>Essence</th>
            <th>Sender id</th>
            <th>Flow id</th>
          </tr>
        </thead>
        <tbody>
          <template v-for="s in nmos.senders" :key="s.channel">
            <tr v-for="kind in ['video', 'audio', 'data']" :key="`${s.channel}-${kind}`">
              <td>{{ kind === "video" ? s.label : "" }}</td>
              <td>{{ kind }}</td>
              <td><code>{{ s[`${kind}_sender`] }}</code></td>
              <td><code>{{ s[`${kind}_flow`] }}</code></td>
            </tr>
          </template>
        </tbody>
      </table>
    </div>
  </div>
  <div v-else class="empty">Loading…</div>
</template>
