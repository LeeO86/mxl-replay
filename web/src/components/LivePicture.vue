<script setup>
// A preview picture. JPEG mode: fetched every `interval` ms while the page is visible (one request at a
// time, no request while hidden), and at once when `bump` changes. WebRTC mode (REPLAY_PREVIEW_MODE=webrtc):
// tile `tile` ("ch1", "cam2") of the page's one mosaic stream, in a box of the tile's aspect ratio.
// Overlays go in the slot.
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import { preview, startPreview, tileCrop } from "../preview.js";
import { state } from "../store.js";

const props = defineProps({
  src: { type: String, required: true },
  interval: { type: Number, default: 1000 },
  bump: { type: null, default: 0 },
  alt: { type: String, default: "" },
  tile: { type: String, default: "" },
});

const webrtc = computed(() => Boolean(props.tile) && state.status?.preview?.mode === "webrtc");
const video = ref(null);
const crop = computed(() => (webrtc.value ? tileCrop(props.tile) : null));
watch(webrtc, (on) => on && startPreview(), { immediate: true });
watch([video, () => preview.stream], ([element, stream]) => {
  if (element && stream && element.srcObject !== stream) {
    element.srcObject = stream;
    element.play().catch(() => {});
  }
});

const url = ref("");
const missing = ref(false);
let busy = false;
let timer = null;

async function load() {
  // The mode comes with the status; the WebRTC mode serves no camera or channel JPEG.
  if (busy || document.hidden || !state.status || webrtc.value) return;
  busy = true;
  try {
    const resp = await fetch(`${props.src}?t=${Date.now()}`, { cache: "no-store" });
    if (resp.ok) {
      const next = URL.createObjectURL(await resp.blob());
      if (url.value) URL.revokeObjectURL(url.value);
      url.value = next;
      missing.value = false;
    } else {
      missing.value = true;
    }
  } catch {
    missing.value = true;
  } finally {
    busy = false;
  }
}

watch(
  () => [props.src, props.bump, Boolean(state.status)],
  () => {
    busy = false;
    load();
  },
);
onMounted(() => {
  load();
  timer = setInterval(load, props.interval);
});
onUnmounted(() => {
  clearInterval(timer);
  if (url.value) URL.revokeObjectURL(url.value);
});
</script>

<template>
  <div class="picture" :style="crop?.box">
    <template v-if="webrtc">
      <video ref="video" :style="crop?.video" muted autoplay playsinline :aria-label="alt"></video>
      <div v-if="!preview.connected" class="picture-empty">Connecting…</div>
    </template>
    <img v-else-if="url && !missing" :src="url" :alt="alt" />
    <div v-else class="picture-empty">No picture</div>
    <slot />
  </div>
</template>
