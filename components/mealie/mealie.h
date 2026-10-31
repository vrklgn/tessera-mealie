#pragma once
#include <string>
#include <utility>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::mealie {

// Someone who asked the app a question (tessera::send) and waits for its answer: a tile or a card. It forgets itself
// in its destructor, so an answer never reaches an object that is gone.
class Asker {
 public:
  virtual ~Asker() = default;
  virtual void answer(JsonObjectConst message) = 0;
};

// What the recipe script answered about a meal (its part "info"): the facts the tile shows under the name and the card
// in its head. Fixed fields, so a Meal is one block in PSRAM (docs/FIRMWARE_API.md, "Memory").
struct Info {
  bool ok = false;
  char description[52] = {};   // the recipe's description, for a recipe chosen by hand (a select has none)
  int servings = 0;
  char yield[32] = {};   // Mealie's own word for the servings ("personer"), "" for none
  char total[32] = {}, prep[32] = {}, cook[32] = {};
  int ingredients = 0, steps = 0;
};

// A meal as a tile shows it, and as the card gets it on a tap: what the entity says, its picture's address, and the
// script's facts.
struct Meal {
  char name[52] = {};          // the recipe's name (the calendar's message), the app cuts a text at 48 bytes
  char description[52] = {};
  char entity[64] = {};        // the calendar, sensor or select it comes from
  char picture[128] = {};      // the picture's address through Home Assistant's image proxy, "" for none
  char date[12] = {};          // the meal plan day, "2026-10-31", for the script; "" for a recipe chosen by hand
  uint32_t start = 0;          // that day, seconds since 1970
  bool fixed = false;          // chosen on the picker: the recipe card keeps it while Home Assistant catches up
  Info info;
};

// One line of text in PSRAM: a recipe of the list to choose from.
struct Line {
  char text[52] = {};
};

// The plugin: a tile of a Mealie calendar ("meal"), a tile of a select entity with a recipe chosen by hand ("pick"),
// the recipe card ("recipe"), the picker ("choose") and an item for the top bar ("today"). It carries the questions of
// its tiles and cards to the app and hands each its answer.
class Mealie : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void on_message(JsonObjectConst message) override;

  void set_message(text_sensor::TextSensor *s) { message_ = s; }
  void set_start(text_sensor::TextSensor *s) { start_ = s; }
  void set_meal(const char *entity) { meal_ = entity; }

  // Ask the recipe script for a meal: its facts (`part` "info") or a page of its "ingredients" or "steps", `per` lines
  // of at most `cols` characters. The number the answer carries, 0 when it could not be sent.
  uint32_t ask(Asker *who, const Meal &meal, const char *part, int page, int per, int cols);
  void forget(Asker *who);
  // The word for a meal from its entity id ("Dinner" for calendar.mealie_dinner, "Recipe" for a select), or the
  // plain word for a meal.
  std::string meal_word(const char *entity) const;

  Meal current;                  // what the tile tapped last showed, for the card it opened
  tessera::List<Line> choices;   // the recipes of the select tile tapped last, for the picker

 protected:
  tessera::BarItem bar() const;
  text_sensor::TextSensor *message_{}, *start_{};
  tessera::Text meal_;   // in PSRAM, as every text a plugin keeps (docs/FIRMWARE_API.md, "Memory")
  std::vector<std::pair<uint32_t, Asker *>> asked_;
};

}  // namespace esphome::mealie
