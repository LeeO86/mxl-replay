<script setup>
// Clip colour: the camera's colour (empty value), a palette, or any colour.
import { CLIP_COLOURS } from "../api.js";

defineProps({
  modelValue: { type: String, default: "" },
  cameraColour: { type: String, default: "" }, // offered as "camera colour" when set
});
defineEmits(["update:modelValue"]);
</script>

<template>
  <div class="swatches">
    <button
      v-if="cameraColour"
      type="button"
      :class="{ active: !modelValue }"
      :style="{ background: cameraColour }"
      title="the camera's colour"
      :aria-pressed="!modelValue"
      @click="$emit('update:modelValue', '')"
    ></button>
    <button
      v-for="c in CLIP_COLOURS"
      :key="c"
      type="button"
      :class="{ active: modelValue === c }"
      :style="{ background: c }"
      :title="c"
      :aria-pressed="modelValue === c"
      @click="$emit('update:modelValue', c)"
    ></button>
    <input type="color" :value="modelValue || cameraColour || '#3b82f6'" title="another colour" @input="$emit('update:modelValue', $event.target.value)" />
  </div>
</template>
