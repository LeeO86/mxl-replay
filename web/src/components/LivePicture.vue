<script setup>
// A JPEG preview fetched every `interval` ms while the page is visible (one request at a time,
// no request while hidden), and at once when `bump` changes. Overlays go in the slot.
import { onMounted, onUnmounted, ref, watch } from "vue";

const props = defineProps({
  src: { type: String, required: true },
  interval: { type: Number, default: 1000 },
  bump: { type: null, default: 0 },
  alt: { type: String, default: "" },
});

const url = ref("");
const missing = ref(false);
let busy = false;
let timer = null;

async function load() {
  if (busy || document.hidden) return;
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
  () => [props.src, props.bump],
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
  <div class="picture">
    <img v-if="url && !missing" :src="url" :alt="alt" />
    <div v-else class="picture-empty">No picture</div>
    <slot />
  </div>
</template>
