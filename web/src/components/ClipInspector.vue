<script setup>
// Clip inspector (SPECIFICATION.md §8.2): IN and OUT frame by frame, speed, modes, end action,
// name, colour and tags. The replay puts a new IN or OUT on the nearest recorded frame.
import { computed, reactive, ref, watch } from "vue";
import ColourPicker from "./ColourPicker.vue";
import Segmented from "./Segmented.vue";
import { api, AUDIO_MODES, END_ACTIONS, fmtNs, fmtSpeed, MOTION_MODES, SPEED_PRESETS } from "../api.js";
import { active, cameraColour, cameraLabel, framePeriod, refreshLists, state } from "../store.js";

const props = defineProps({ clip: { type: Object, default: null } });
const emit = defineEmits(["close"]);

const dialog = ref(null);
const message = ref("");
const busy = ref(false);
const draft = reactive({ name: "", inFrames: 0, outFrames: 0, speed: 100, motion: "", audio: "", end: "", colour: "", tags: "" });

watch(
  () => props.clip?.id,
  (id) => {
    if (!id) {
      dialog.value?.close();
      return;
    }
    const c = props.clip;
    Object.assign(draft, {
      name: c.name,
      inFrames: 0,
      outFrames: 0,
      speed: Math.round(c.speed * 100),
      motion: c.motion,
      audio: c.audio,
      end: c.end,
      colour: c.colour,
      tags: c.tags,
    });
    message.value = "";
    dialog.value.showModal();
  },
);

const period = computed(() => framePeriod(props.clip?.camera));
const framesPerSecond = computed(() => Math.round(1e9 / period.value));
const newIn = computed(() => props.clip.in_ns + draft.inFrames * period.value);
const newOut = computed(() => props.clip.out_ns + draft.outFrames * period.value);
const length = computed(() => newOut.value - newIn.value);
const signed = (n) => (n > 0 ? `+${n}` : String(n));
const presets = SPEED_PRESETS.map((s) => ({ value: Math.round(s * 100), label: fmtSpeed(s) }));
const canTakeMarks = computed(() => active.value?.has_in && active.value?.has_out && active.value.out_ns > active.value.in_ns);

function nudge(which, frames) {
  draft[which] += frames;
}
// IN and OUT from the selected channel's marks.
function takeMarks() {
  draft.inFrames = Math.round((active.value.in_ns - props.clip.in_ns) / period.value);
  draft.outFrames = Math.round((active.value.out_ns - props.clip.out_ns) / period.value);
}

async function save() {
  const c = props.clip;
  const body = {};
  if (draft.name.trim() && draft.name.trim() !== c.name) body.name = draft.name.trim();
  if (draft.inFrames) body.in_ns = Math.round(newIn.value);
  if (draft.outFrames) body.out_ns = Math.round(newOut.value);
  if (draft.speed !== Math.round(c.speed * 100)) body.speed = draft.speed / 100;
  for (const key of ["motion", "audio", "end", "tags"]) if (draft[key] !== c[key]) body[key] = draft[key];
  // "Camera colour" stores the camera's colour itself.
  const colour = draft.colour || cameraColour(c.camera);
  if (colour !== c.colour) body.colour = colour;
  if (!Object.keys(body).length) {
    emit("close");
    return;
  }
  busy.value = true;
  try {
    await api.patch(`/api/v1/clips/${encodeURIComponent(c.id)}`, body);
    state.notice = `Saved ${draft.name.trim() || c.name}.`;
    await refreshLists();
    emit("close");
  } catch (e) {
    message.value = e.message;
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <dialog ref="dialog" class="wide" @close="emit('close')">
    <form v-if="clip" @submit.prevent="save">
      <h3>Edit clip <span class="muted small">{{ clip.id }} · {{ cameraLabel(clip.camera) }}</span></h3>
      <label for="insp-name">Name</label>
      <input id="insp-name" v-model="draft.name" maxlength="120" />

      <div class="grid" style="grid-template-columns: 1fr 1fr; margin-top: 0.4rem">
        <div v-for="mark in ['in', 'out']" :key="mark">
          <label>{{ mark.toUpperCase() }}</label>
          <div class="num" style="font-family: ui-monospace, monospace; font-size: 1.05rem">
            {{ mark === "in" ? clip.in_tc : clip.out_tc }}
            <span v-if="draft[`${mark}Frames`]" class="pill warn">{{ signed(draft[`${mark}Frames`]) }} f</span>
          </div>
          <div class="seg" style="margin-top: 0.3rem">
            <button type="button" @click="nudge(`${mark}Frames`, -framesPerSecond)">−1 s</button>
            <button type="button" @click="nudge(`${mark}Frames`, -1)">−1 f</button>
            <button type="button" @click="nudge(`${mark}Frames`, 1)">+1 f</button>
            <button type="button" @click="nudge(`${mark}Frames`, framesPerSecond)">+1 s</button>
          </div>
        </div>
      </div>
      <div class="row" style="align-items: center; margin-top: 0.5rem">
        <span class="small">Length {{ fmtNs(length) }} · plays {{ fmtNs(length / Math.max(0.01, draft.speed / 100)) }}</span>
        <button type="button" class="btn small secondary" style="flex: none" :disabled="!canTakeMarks" @click="takeMarks">
          Take IN/OUT from {{ active?.label }}
        </button>
      </div>
      <div v-if="length < 0" class="msg err">OUT is before IN.</div>

      <label>Speed {{ draft.speed }}%</label>
      <div class="row" style="align-items: center">
        <input v-model.number="draft.speed" type="number" min="0" max="200" step="1" style="flex: 0 0 6rem" aria-label="Speed in percent" />
        <Segmented v-model="draft.speed" :options="presets" label="Speed preset" />
      </div>
      <div class="grid" style="grid-template-columns: 1fr 1fr 1fr; margin-top: 0.2rem">
        <div>
          <label for="insp-motion">Motion</label>
          <select id="insp-motion" v-model="draft.motion"><option v-for="m in MOTION_MODES" :key="m.value" :value="m.value">{{ m.label }}</option></select>
        </div>
        <div>
          <label for="insp-audio">Audio</label>
          <select id="insp-audio" v-model="draft.audio"><option v-for="m in AUDIO_MODES" :key="m.value" :value="m.value">{{ m.label }}</option></select>
        </div>
        <div>
          <label for="insp-end">At the end</label>
          <select id="insp-end" v-model="draft.end"><option v-for="m in END_ACTIONS" :key="m.value" :value="m.value">{{ m.label }}</option></select>
        </div>
      </div>
      <label>Colour</label>
      <ColourPicker v-model="draft.colour" :camera-colour="cameraColour(clip.camera)" />
      <label for="insp-tags">Tags</label>
      <input id="insp-tags" v-model="draft.tags" placeholder="comma separated" />
      <div v-if="message" class="msg err">{{ message }}</div>
      <div class="actions">
        <button type="button" class="btn secondary" @click="emit('close')">Cancel</button>
        <button type="submit" class="btn" :disabled="busy || length < 0 || draft.speed < 0 || draft.speed > 200">Save</button>
      </div>
    </form>
  </dialog>
</template>
