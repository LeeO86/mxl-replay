<script setup>
// Create clip (SPECIFICATION.md §7): the selected channel's IN to OUT with a name, all angles,
// colour and tags. When clips already keep too much of the storage, the replay asks first (409).
import { computed, nextTick, ref, watch } from "vue";
import ColourPicker from "./ColourPicker.vue";
import { api, fmtNs, fmtSpeed } from "../api.js";
import { active, cameraColour, cameraLabel, refreshLists, state } from "../store.js";

const props = defineProps({ open: { type: Boolean, default: false } });
const emit = defineEmits(["close"]);

const dialog = ref(null);
const nameField = ref(null);
const name = ref("");
const allAngles = ref(false);
const colour = ref("");
const tags = ref("");
const conflict = ref("");
const message = ref("");
const busy = ref(false);

watch(
  () => props.open,
  async (open) => {
    if (!open) {
      dialog.value?.close();
      return;
    }
    name.value = "";
    allAngles.value = false;
    colour.value = "";
    tags.value = "";
    conflict.value = "";
    message.value = "";
    dialog.value.showModal();
    await nextTick();
    nameField.value?.focus();
  },
);

const length = computed(() => (active.value ? active.value.out_ns - active.value.in_ns : 0));

async function create(force) {
  busy.value = true;
  message.value = "";
  try {
    const created = await api.post("/api/v1/clips", {
      channel: state.channel,
      name: name.value.trim(),
      all_angles: allAngles.value,
      force,
      colour: colour.value,
      tags: tags.value.trim(),
    });
    state.notice = allAngles.value ? `Created ${created.id} and one clip for each other camera.` : `Created ${created.id}.`;
    await refreshLists();
    emit("close");
  } catch (e) {
    if (e.status === 409) conflict.value = e.message;
    else message.value = e.message;
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <dialog ref="dialog" @close="emit('close')">
    <form @submit.prevent="create(false)">
      <h3>Create clip</h3>
      <dl v-if="active" class="kv">
        <dt>Channel</dt>
        <dd>{{ active.label }}</dd>
        <dt>Camera</dt>
        <dd>{{ allAngles ? "every camera" : cameraLabel(active.camera) }}</dd>
        <dt>IN</dt>
        <dd>{{ active.in_tc }}</dd>
        <dt>OUT</dt>
        <dd>{{ active.out_tc }}</dd>
        <dt>Length</dt>
        <dd>{{ fmtNs(length) }}</dd>
        <dt>Speed</dt>
        <dd>{{ fmtSpeed(active.target_speed ?? active.speed) }} (the channel's speed; change it in the clip later)</dd>
      </dl>
      <label for="clip-name">Name</label>
      <input id="clip-name" ref="nameField" v-model="name" placeholder="automatic: clip-<number>" maxlength="120" />
      <label class="check"><input v-model="allAngles" type="checkbox" /> All angles: one clip per camera with the same IN and OUT</label>
      <label>Colour</label>
      <ColourPicker v-model="colour" :camera-colour="active ? cameraColour(active.camera) : ''" />
      <label for="clip-tags">Tags</label>
      <input id="clip-tags" v-model="tags" placeholder="comma separated, e.g. goal, home team" />
      <div v-if="conflict" class="warnbox">
        <span>{{ conflict }}</span>
        <button type="button" class="btn small danger" :disabled="busy" @click="create(true)">Create anyway</button>
      </div>
      <div v-if="message" class="msg err">{{ message }}</div>
      <div class="actions">
        <button type="button" class="btn secondary" @click="emit('close')">Cancel</button>
        <button type="submit" class="btn" :disabled="busy">Create</button>
      </div>
    </form>
  </dialog>
</template>
