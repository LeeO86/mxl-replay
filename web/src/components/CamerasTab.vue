<script setup>
// Cameras and storage (SPECIFICATION.md §4, §8.3).
import { computed } from "vue";
import LivePicture from "./LivePicture.vue";
import Pill from "./Pill.vue";
import { fmtBytes, fmtNs, fmtSeconds } from "../api.js";
import { cameras, state } from "../store.js";

const s = computed(() => state.status);
const routeKind = (route) => ({ running: "ok", waiting: "warn", unsupported: "bad" })[route] || "neutral";
const routeText = (route) => route || "not connected";
function recordPill(cam) {
  if (!cam.record) return { text: "not recording (off)", kind: "neutral" };
  return cam.recording ? { text: "recording", kind: "ok" } : { text: "no signal", kind: "bad" };
}
</script>

<template>
  <div v-if="s" class="panel">
    <h3>Storage</h3>
    <dl class="kv">
      <dt>Free</dt>
      <dd>{{ fmtBytes(s.free_bytes) }}</dd>
      <dt>Write speed at start</dt>
      <dd>{{ fmtBytes(s.storage_bps) }}/s</dd>
      <dt>Format</dt>
      <dd>{{ s.format }}</dd>
      <dt>JPEG</dt>
      <dd>{{ s.jpeg }}, {{ s.jpeg_bit_depth }}-bit 4:2:2</dd>
      <dt>Interpolation</dt>
      <dd>{{ s.gpu ? `${s.flow}, preset ${s.preset}` : "off (no GPU): interpolate plays as blend" }}</dd>
    </dl>
  </div>

  <div class="grid" style="grid-template-columns: repeat(auto-fill, minmax(280px, 1fr))">
    <div v-for="cam in cameras" :key="cam.index" class="panel cam-panel">
      <h3>
        <span class="swatch big" :style="{ background: cam.colour }" :title="cam.colour"></span>
        {{ cam.label }}
        <span class="spacer"></span>
        <Pill :text="recordPill(cam).text" :kind="recordPill(cam).kind" />
      </h3>
      <LivePicture :src="`/api/v1/cameras/${cam.index}/preview.jpg`" :interval="2000" :alt="cam.label" />
      <dl class="kv">
        <dt>Video input</dt>
        <dd>
          <Pill v-for="(route, i) in cam.video_states" :key="i" :text="`${cam.phases > 1 ? `phase ${i + 1}: ` : ''}${routeText(route)}`" :kind="routeKind(route)" />
        </dd>
        <dt>Audio input</dt>
        <dd><Pill :text="cam.audio_record ? routeText(cam.audio_state) : 'not recorded'" :kind="cam.audio_record ? routeKind(cam.audio_state) : 'neutral'" /></dd>
        <dt>Buffer</dt>
        <dd>keeps {{ fmtSeconds(cam.buffer_hours * 3600) }}, holds {{ cam.newest_ns ? fmtNs(cam.newest_ns - cam.oldest_ns) : "nothing yet" }}</dd>
        <dt>Frames stored</dt>
        <dd>{{ cam.frames }}</dd>
        <dt>Dropped</dt>
        <dd :class="{ muted: !cam.dropped }">{{ cam.dropped }}</dd>
        <dt>Phases</dt>
        <dd>{{ cam.phases }}<span v-if="cam.phases > 1"> (missing {{ cam.phase_missing }})</span></dd>
        <dt>High frame rate</dt>
        <dd>{{ cam.hfr_factor > 1 ? `×${cam.hfr_factor}` : "no" }}</dd>
        <dt>Scaled</dt>
        <dd>
          <Pill v-if="cam.scaled" text="yes: not the house raster" kind="warn" />
          <span v-else>no</span>
        </dd>
        <dt>Disk</dt>
        <dd>{{ fmtBytes(cam.disk_bytes) }} in {{ cam.segments }} segments</dd>
        <dt>Kept by clips</dt>
        <dd>{{ fmtBytes(cam.protected_bytes) }}</dd>
      </dl>
    </div>
  </div>
  <div v-if="!cameras.length" class="empty">Waiting for the replay status…</div>
</template>
