#include "simply_stage.h"

#include "simply_window.h"
#include "simply_res.h"
#include "simply_msg.h"
#include "simply_window_stack.h"

#include "simply.h"

#include "util/color.h"
#include "util/compat.h"
#include "util/graphics.h"
#include "util/inverter_layer.h"
#include "util/memory.h"
#include "util/string.h"
#include "util/window.h"

#include <pebble.h>

typedef Packet StageClearPacket;

typedef struct ElementInsertPacket ElementInsertPacket;

struct __attribute__((__packed__)) ElementInsertPacket {
  Packet packet;
  uint32_t id;
  SimplyElementType type:8;
  uint16_t index;
};

typedef struct ElementRemovePacket ElementRemovePacket;

struct __attribute__((__packed__)) ElementRemovePacket {
  Packet packet;
  uint32_t id;
};

typedef struct ElementCommonPacket ElementCommonPacket;

struct __attribute__((__packed__)) ElementCommonPacket {
  Packet packet;
  uint32_t id;
  GRect frame;
  GColor8 background_color;
  GColor8 border_color;
};

typedef struct ElementRadiusPacket ElementRadiusPacket;

struct __attribute__((__packed__)) ElementRadiusPacket {
  Packet packet;
  uint32_t id;
  uint16_t radius;
};

typedef struct ElementTextPacket ElementTextPacket;

struct __attribute__((__packed__)) ElementTextPacket {
  Packet packet;
  uint32_t id;
  TimeUnits time_units:8;
  char text[];
};

typedef struct ElementTextStylePacket ElementTextStylePacket;

struct __attribute__((__packed__)) ElementTextStylePacket {
  Packet packet;
  uint32_t id;
  GColor8 color;
  GTextOverflowMode overflow_mode:8;
  GTextAlignment alignment:8;
  uint32_t custom_font;
  char system_font[];
};

typedef struct ElementImagePacket ElementImagePacket;

struct __attribute__((__packed__)) ElementImagePacket {
  Packet packet;
  uint32_t id;
  uint32_t image;
  GCompOp compositing:8;
};

typedef struct ElementAnimatePacket ElementAnimatePacket;

struct __attribute__((__packed__)) ElementAnimatePacket {
  Packet packet;
  uint32_t id;
  GRect frame;
  uint32_t duration;
  AnimationCurve curve:8;
};

typedef struct ElementAnimateDonePacket ElementAnimateDonePacket;

struct __attribute__((__packed__)) ElementAnimateDonePacket {
  Packet packet;
  uint32_t id;
};

static void simply_stage_clear(SimplyStage *self);

static void simply_stage_update(SimplyStage *self);
static void simply_stage_update_ticker(SimplyStage *self);

static SimplyElementCommon* simply_stage_auto_element(SimplyStage *self, uint32_t id, SimplyElementType type);
static SimplyElementCommon* simply_stage_insert_element(SimplyStage *self, int index, SimplyElementCommon *element);
static SimplyElementCommon* simply_stage_remove_element(SimplyStage *self, SimplyElementCommon *element);

static void simply_stage_set_element_frame(SimplyStage *self, SimplyElementCommon *element, GRect frame);

static SimplyAnimation *simply_stage_animate_element(SimplyStage *self,
    SimplyElementCommon *element, SimplyAnimation* animation, GRect to_frame);

static bool send_animate_element_done(SimplyMsg *self, uint32_t id) {
  ElementAnimateDonePacket packet = {
    .packet.type = CommandElementAnimateDone,
    .packet.length = sizeof(packet),
    .id = id,
  };
  return simply_msg_send_packet(&packet.packet);
}

static bool id_filter(List1Node *node, void *data) {
  return (((SimplyElementCommon*) node)->id == (uint32_t)(uintptr_t) data);
}

static bool animation_filter(List1Node *node, void *data) {
  return (((SimplyAnimation*) node)->animation == (PropertyAnimation*) data);
}

static bool animation_element_filter(List1Node *node, void *data) {
  return (((SimplyAnimation*) node)->element == (SimplyElementCommon*) data);
}

static void destroy_element(SimplyStage *self, SimplyElementCommon *element) {
  if (!element) { return; }
  list1_remove(&self->stage_layer.elements, &element->node);
  switch (element->type) {
    default: break;
    case SimplyElementTypeText:
      free(((SimplyElementText*) element)->text);
      break;
    case SimplyElementTypeInverter:
      inverter_layer_destroy(((SimplyElementInverter*) element)->inverter_layer);
      break;
  }
  free(element);
}

static void destroy_animation(SimplyStage *self, SimplyAnimation *animation) {
  if (!animation) { return; }
  property_animation_destroy(animation->animation);
  list1_remove(&self->stage_layer.animations, &animation->node);
  free(animation);
}

void simply_stage_clear(SimplyStage *self) {
  simply_window_action_bar_clear(&self->window);

  while (self->stage_layer.elements) {
    destroy_element(self, (SimplyElementCommon*) self->stage_layer.elements);
  }

  while (self->stage_layer.animations) {
    destroy_animation(self, (SimplyAnimation*) self->stage_layer.animations);
  }

  simply_stage_update_ticker(self);
}

static void rect_element_draw_background(GContext *ctx, SimplyStage *self, SimplyElementRect *element) {
  if (element->background_color.a) {
    graphics_context_set_fill_color(ctx, gcolor8_get(element->background_color));
    graphics_fill_rect(ctx, element->frame, element->radius, GCornersAll);
  }
}

static void rect_element_draw_border(GContext *ctx, SimplyStage *self, SimplyElementRect *element) {
  if (element->border_color.a) {
    graphics_context_set_stroke_color(ctx, gcolor8_get(element->border_color));
    graphics_draw_round_rect(ctx, element->frame, element->radius);
  }
}

static void rect_element_draw(GContext *ctx, SimplyStage *self, SimplyElementRect *element) {
  rect_element_draw_background(ctx, self, element);
  rect_element_draw_border(ctx, self, element);
}

static void circle_element_draw(GContext *ctx, SimplyStage *self, SimplyElementCircle *element) {
  if (element->background_color.a) {
    graphics_context_set_fill_color(ctx, gcolor8_get(element->background_color));
    graphics_fill_circle(ctx, element->frame.origin, element->radius);
  }
  if (element->border_color.a) {
    graphics_context_set_stroke_color(ctx, gcolor8_get(element->border_color));
    graphics_draw_circle(ctx, element->frame.origin, element->radius);
  }
}

static char *format_time(char *format) {
  time_t now = time(NULL);
  struct tm* tm = localtime(&now);
  static char time_text[256];
  strftime(time_text, sizeof(time_text), format, tm);
  return time_text;
}

static void text_element_draw(GContext *ctx, SimplyStage *self, SimplyElementText *element) {
  rect_element_draw(ctx, self, (SimplyElementRect*) element);
  char *text = element->text;
  if (element->text_color.a && is_string(text)) {
    if (element->time_units) {
      text = format_time(text);
    }
    GFont font = element->font ? element->font : fonts_get_system_font(FONT_KEY_GOTHIC_14);
    graphics_context_set_text_color(ctx, gcolor8_get(element->text_color));
    graphics_draw_text(ctx, text, font, element->frame, element->overflow_mode, element->alignment, NULL);
  }
}

static void image_element_draw(GContext *ctx, SimplyStage *self, SimplyElementImage *element) {
  graphics_context_set_compositing_mode(ctx, element->compositing);
  rect_element_draw_background(ctx, self, (SimplyElementRect*) element);
  SimplyImage *image = simply_res_get_image(self->window.simply->res, element->image);
  if (image && image->bitmap) {
    GRect frame = element->frame;
    if (frame.size.w == 0 && frame.size.h == 0) {
      frame = gbitmap_get_bounds(image->bitmap);
    }
    graphics_draw_bitmap_centered(ctx, image->bitmap, frame);
  }
  rect_element_draw_border(ctx, self, (SimplyElementRect*) element);
  graphics_context_set_compositing_mode(ctx, GCompOpAssign);
}

static void layer_update_callback(Layer *layer, GContext *ctx) {
  SimplyStage *self = *(void**) layer_get_data(layer);

  GRect frame = layer_get_frame(layer);
  frame.origin = scroll_layer_get_content_offset(self->window.scroll_layer);
  frame.origin.x = -frame.origin.x;
  frame.origin.y = -frame.origin.y;

  graphics_context_set_antialiased(ctx, true);

  graphics_context_set_fill_color(ctx, gcolor8_get(self->window.background_color));
  graphics_fill_rect(ctx, frame, 0, GCornerNone);

  SimplyElementCommon *element = (SimplyElementCommon*) self->stage_layer.elements;
  while (element) {
    int16_t max_y = element->frame.origin.y + element->frame.size.h;
    if (max_y > frame.size.h) {
      frame.size.h = max_y;
    }
    switch (element->type) {
      case SimplyElementTypeNone:
        break;
      case SimplyElementTypeRect:
        rect_element_draw(ctx, self, (SimplyElementRect*) element);
        break;
      case SimplyElementTypeCircle:
        circle_element_draw(ctx, self, (SimplyElementCircle*) element);
        break;
      case SimplyElementTypeText:
        text_element_draw(ctx, self, (SimplyElementText*) element);
        break;
      case SimplyElementTypeImage:
        image_element_draw(ctx, self, (SimplyElementImage*) element);
        break;
      case SimplyElementTypeInverter:
        break;
    }
    element = (SimplyElementCommon*) element->node.next;
  }

  if (self->window.is_scrollable) {
    frame.origin = GPointZero;
    layer_set_frame(layer, frame);
    scroll_layer_set_content_size(self->window.scroll_layer, frame.size);
  }
}

static SimplyElementCommon *alloc_element(SimplyElementType type) {
  switch (type) {
    case SimplyElementTypeNone: return NULL;
    case SimplyElementTypeRect: return malloc0(sizeof(SimplyElementRect));
    case SimplyElementTypeCircle: return malloc0(sizeof(SimplyElementCircle));
    case SimplyElementTypeText: return malloc0(sizeof(SimplyElementText));
    case SimplyElementTypeImage: return malloc0(sizeof(SimplyElementImage));
    case SimplyElementTypeInverter: {
      SimplyElementInverter *element = malloc0(sizeof(SimplyElementInverter));
      if (!element) {
        return NULL;
      }
      element->inverter_layer = inverter_layer_create(GRect(0, 0, 0, 0));
      return &element->common;
    }
  }
  return NULL;
}

SimplyElementCommon *simply_stage_auto_element(SimplyStage *self, uint32_t id, SimplyElementType type) {
  if (!id) {
    return NULL;
  }
  SimplyElementCommon *element = (SimplyElementCommon*) list1_find(
      self->stage_layer.elements, id_filter, (void*)(uintptr_t) id);
  if (element) {
    return element;
  }
  while (!(element = alloc_element(type))) {
    if (!simply_res_evict_image(self->window.simply->res)) {
      return NULL;
    }
  }
  element->id = id;
  element->type = type;
  return element;
}

SimplyElementCommon *simply_stage_insert_element(SimplyStage *self, int index, SimplyElementCommon *element) {
  simply_stage_remove_element(self, element);
  switch (element->type) {
    default: break;
    case SimplyElementTypeInverter:
      layer_add_child(self->stage_layer.layer,
          inverter_layer_get_layer(((SimplyElementInverter*) element)->inverter_layer));
      break;
  }
  return (SimplyElementCommon*) list1_insert(&self->stage_layer.elements, index, &element->node);
}

SimplyElementCommon *simply_stage_remove_element(SimplyStage *self, SimplyElementCommon *element) {
  switch (element->type) {
    default: break;
    case SimplyElementTypeInverter:
      layer_remove_from_parent(inverter_layer_get_layer(((SimplyElementInverter*) element)->inverter_layer));
      break;
  }
  return (SimplyElementCommon*) list1_remove(&self->stage_layer.elements, &element->node);
}

void simply_stage_set_element_frame(SimplyStage *self, SimplyElementCommon *element, GRect frame) {
  grect_standardize(&frame);
  element->frame = frame;
  switch (element->type) {
    default: break;
    case SimplyElementTypeInverter: {
      Layer *layer = inverter_layer_get_layer(((SimplyElementInverter*) element)->inverter_layer);
      layer_set_frame(layer, element->frame);
      break;
    }
  }
}

static void element_frame_setter(void *subject, GRect frame) {
  SimplyAnimation *animation = subject;
  simply_stage_set_element_frame(animation->stage, animation->element, frame);
  simply_stage_update(animation->stage);
}

static GRect element_frame_getter(void *subject) {
  SimplyAnimation *animation = subject;
  return animation->element->frame;
}

static void animation_stopped(Animation *base_animation, bool finished, void *context) {
  SimplyStage *self = context;
  SimplyAnimation *animation = (SimplyAnimation*) list1_find(
      self->stage_layer.animations, animation_filter, base_animation);
  if (!animation) {
    return;
  }
  SimplyElementCommon *element = animation->element;
  destroy_animation(self, animation);
  send_animate_element_done(self->window.simply->msg, element->id);
}

SimplyAnimation *simply_stage_animate_element(SimplyStage *self,
    SimplyElementCommon *element, SimplyAnimation* animation, GRect to_frame) {
  if (!animation) {
    return NULL;
  }
  SimplyAnimation *prev_animation = (SimplyAnimation*) list1_find(
      self->stage_layer.animations, animation_element_filter, element);
  if (prev_animation) {
    animation_unschedule((Animation*) prev_animation->animation);
  }

  animation->stage = self;
  animation->element = element;

  static const PropertyAnimationImplementation implementation = {
    .base = {
      .update = (AnimationUpdateImplementation) property_animation_update_grect,
      .teardown = (AnimationTeardownImplementation) animation_destroy,
    },
    .accessors = {
      .setter = { .grect = (const GRectSetter) element_frame_setter },
      .getter = { .grect = (const GRectGetter) element_frame_getter },
    },
  };

  PropertyAnimation *property_animation = property_animation_create(&implementation, animation, NULL, NULL);
  if (!property_animation) {
    free(animation);
    return NULL;
  }

  property_animation_set_from_grect(property_animation, &element->frame);
  property_animation_set_to_grect(property_animation, &to_frame);

  animation->animation = property_animation;
  list1_append(&self->stage_layer.animations, &animation->node);

  Animation *base_animation = (Animation*) property_animation;
  animation_set_duration(base_animation, animation->duration);
  animation_set_curve(base_animation, animation->curve);
  animation_set_handlers(base_animation, (AnimationHandlers) {
    .stopped = animation_stopped,
  }, self);

  animation_schedule(base_animation);

  return animation;
}

static void window_load(Window *window) {
  SimplyStage * const self = window_get_user_data(window);

  simply_window_load(&self->window);

  Layer * const window_layer = window_get_root_layer(window);
  const GRect frame = { .size = layer_get_frame(window_layer).size };

  Layer * const layer = layer_create_with_data(frame, sizeof(void *));
  self->stage_layer.layer = layer;
  *(void**) layer_get_data(layer) = self;
  layer_set_update_proc(layer, layer_update_callback);
  scroll_layer_add_child(self->window.scroll_layer, layer);
}

static void window_appear(Window *window) {
  SimplyStage *self = window_get_user_data(window);
  simply_window_appear(&self->window);

  simply_stage_update_ticker(self);
}

static void window_disappear(Window *window) {
  SimplyStage *self = window_get_user_data(window);
  if (simply_window_disappear(&self->window)) {
    simply_res_clear(self->window.simply->res);
    simply_stage_clear(self);
  }
}

static void window_unload(Window *window) {
  SimplyStage *self = window_get_user_data(window);

  layer_destroy(self->stage_layer.layer);
  self->window.layer = self->stage_layer.layer = NULL;

  simply_window_unload(&self->window);
}

void simply_stage_update(SimplyStage *self) {
  if (self->stage_layer.layer) {
    layer_mark_dirty(self->stage_layer.layer);
  }
}

static void handle_tick(struct tm *tick_time, TimeUnits units_changed) {
  window_stack_schedule_top_window_render();
}

void simply_stage_update_ticker(SimplyStage *self) {
  TimeUnits units = 0;

  SimplyElementCommon *element = (SimplyElementCommon*) self->stage_layer.elements;
  while (element) {
    if (element->type == SimplyElementTypeText) {
      units |= ((SimplyElementText*) element)->time_units;
    }
    element = (SimplyElementCommon*) element->node.next;
  }

  if (units) {
    tick_timer_service_subscribe(units, handle_tick);
  } else {
    tick_timer_service_unsubscribe();
  }
}

static void handle_stage_clear_packet(Simply *simply, Packet *data) {
  simply_stage_clear(simply->stage);
}

static void handle_element_insert_packet(Simply *simply, Packet *data) {
  ElementInsertPacket *packet = (ElementInsertPacket*) data;
  SimplyElementCommon *element = simply_stage_auto_element(simply->stage, packet->id, packet->type);
  if (!element) {
    return;
  }
  simply_stage_insert_element(simply->stage, packet->index, element);
  simply_stage_update(simply->stage);
}

static void handle_element_remove_packet(Simply *simply, Packet *data) {
  ElementInsertPacket *packet = (ElementInsertPacket*) data;
  SimplyElementCommon *element = simply_stage_get_element(simply->stage, packet->id);
  if (!element) {
    return;
  }
  simply_stage_remove_element(simply->stage, element);
  simply_stage_update(simply->stage);
}

static void handle_element_common_packet(Simply *simply, Packet *data) {
  ElementCommonPacket *packet = (ElementCommonPacket*) data;
  SimplyElementCommon *element = simply_stage_get_element(simply->stage, packet->id);
  if (!element) {
    return;
  }
  simply_stage_set_element_frame(simply->stage, element, packet->frame);
  element->background_color = packet->background_color;
  element->border_color = packet->border_color;
  simply_stage_update(simply->stage);
}

static void handle_element_radius_packet(Simply *simply, Packet *data) {
  ElementRadiusPacket *packet = (ElementRadiusPacket*) data;
  SimplyElementRect *element = (SimplyElementRect*) simply_stage_get_element(simply->stage, packet->id);
  if (!element) {
    return;
  }
  element->radius = packet->radius;
  simply_stage_update(simply->stage);
};

static void handle_element_text_packet(Simply *simply, Packet *data) {
  ElementTextPacket *packet = (ElementTextPacket*) data;
  SimplyElementText *element = (SimplyElementText*) simply_stage_get_element(simply->stage, packet->id);
  if (!element) {
    return;
  }
  if (element->time_units != packet->time_units) {
    element->time_units = packet->time_units;
    simply_stage_update_ticker(simply->stage);
  }
  strset(&element->text, packet->text);
  simply_stage_update(simply->stage);
}

static void handle_element_text_style_packet(Simply *simply, Packet *data) {
  ElementTextStylePacket *packet = (ElementTextStylePacket*) data;
  SimplyElementText *element = (SimplyElementText*) simply_stage_get_element(simply->stage, packet->id);
  if (!element) {
    return;
  }
  element->text_color = packet->color;
  element->overflow_mode = packet->overflow_mode;
  element->alignment = packet->alignment;
  if (packet->custom_font) {
    element->font = simply_res_get_font(simply->res, packet->custom_font);
  } else if (packet->system_font[0]) {
    element->font = fonts_get_system_font(packet->system_font);
  }
  simply_stage_update(simply->stage);
}

static void handle_element_image_packet(Simply *simply, Packet *data) {
  ElementImagePacket *packet = (ElementImagePacket*) data;
  SimplyElementImage *element = (SimplyElementImage*) simply_stage_get_element(simply->stage, packet->id);
  if (!element) {
    return;
  }
  element->image = packet->image;
  element->compositing = packet->compositing;
  simply_stage_update(simply->stage);
}

static void handle_element_animate_packet(Simply *simply, Packet *data) {
  ElementAnimatePacket *packet = (ElementAnimatePacket*) data;
  SimplyElementCommon *element = simply_stage_get_element(simply->stage, packet->id);
  if (!element) {
    return;
  }
  SimplyAnimation *animation = NULL;
  while (!(animation = malloc0(sizeof(*animation)))) {
    if (!simply_res_evict_image(simply->res)) {
      return;
    }
  }
  animation->duration = packet->duration;
  animation->curve = packet->curve;
  simply_stage_animate_element(simply->stage, element, animation, packet->frame);
}

bool simply_stage_handle_packet(Simply *simply, Packet *packet) {
  switch (packet->type) {
    case CommandStageClear:
      handle_stage_clear_packet(simply, packet);
      return true;
    case CommandElementInsert:
      handle_element_insert_packet(simply, packet);
      return true;
    case CommandElementRemove:
      handle_element_remove_packet(simply, packet);
      return true;
    case CommandElementCommon:
      handle_element_common_packet(simply, packet);
      return true;
    case CommandElementRadius:
      handle_element_radius_packet(simply, packet);
      return true;
    case CommandElementText:
      handle_element_text_packet(simply, packet);
      return true;
    case CommandElementTextStyle:
      handle_element_text_style_packet(simply, packet);
      return true;
    case CommandElementImage:
      handle_element_image_packet(simply, packet);
      return true;
    case CommandElementAnimate:
      handle_element_animate_packet(simply, packet);
      return true;
  }
  return false;
}

SimplyStage *simply_stage_create(Simply *simply) {
  SimplyStage *self = malloc(sizeof(*self));
  *self = (SimplyStage) { .window.simply = simply };

  static const WindowHandlers s_window_handlers = {
    .load = window_load,
    .appear = window_appear,
    .disappear = window_disappear,
    .unload = window_unload,
  };
  self->window.window_handlers = &s_window_handlers;

  simply_window_init(&self->window, simply);
  simply_window_set_background_color(&self->window, GColor8Black);

  return self;
}

void simply_stage_destroy(SimplyStage *self) {
  if (!self) {
    return;
  }

  simply_window_deinit(&self->window);

  free(self);
}
