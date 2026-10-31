#include "mealie.h"
#include "esphome/core/hal.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome::mealie {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

// The Home Assistant script of the plugin's package (homeassistant/), asked through the app.
static const char *const SCRIPT = "call_service:script.tessera_mealie";
// A script that did not answer, or is not there, is asked again after this long.
static constexpr uint32_t RETRY_MS = 15u * 60u * 1000u;
// Lines a card asks for at most in one page: a page is one answer of at most about 3 KB.
static constexpr int MAX_LINES = 24;
// The widest line the script writes, in characters; the app cuts a text at 48 bytes.
static constexpr int MAX_COLS = 46;
// Recipes the picker keeps at most: the select's options, as many as the tile's data carries.
static constexpr int MAX_CHOICES = 64;

static void put(char *dst, size_t size, const char *src) { snprintf(dst, size, "%s", src ? src : ""); }
static bool starts(const char *text, const char *head) { return text && strncmp(text, head, strlen(head)) == 0; }
// A select entity: a recipe chosen by hand, in Home Assistant (input_select.x, select.x).
static bool is_select(const char *entity) { return starts(entity, "input_select.") || starts(entity, "select."); }

// ---- Days ----

// Days since 1970 of a civil date, so two dates subtract to whole days (Howard Hinnant's days_from_civil).
static int32_t civil_days(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  return era * 146097 + static_cast<int32_t>(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
}

// The day of "2026-10-31", "2026-10-31 00:00:00" or "2026-10-31T00:00:00+01:00" as days since 1970; INT32_MIN for none.
static int32_t day_of(const char *text) {
  int y = 0, m = 0, d = 0;
  if (!text || strlen(text) < 10 || sscanf(text, "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31)
    return INT32_MIN;
  return civil_days(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
}

// Today on the screen's own calendar, as days since 1970; INT32_MIN while the clock is not set.
static int32_t today() {
  const uint32_t now = tessera::epoch();
  if (!now) return INT32_MIN;
  const tessera::LocalTime t = tessera::local_time(now);
  return civil_days(t.year, static_cast<unsigned>(t.month), static_cast<unsigned>(t.day));
}

// A moment as the script wants the meal plan day: "2026-10-31", "" for none.
static void date_of(uint32_t epoch, char *out, size_t size) {
  if (!epoch) {
    out[0] = 0;
    return;
  }
  const tessera::LocalTime t = tessera::local_time(epoch);
  snprintf(out, size, "%04d-%02d-%02d", t.year, t.month, t.day);
}

// "Today", "Tomorrow", "In 3 days", else the day itself ("Fri 9 Oct"); "" for no day.
static std::string when_text(const tessera::Plugin *plugin, uint32_t start) {
  if (!start) return "";
  const int32_t days = tessera::days_from_today(start);
  if (days == INT32_MIN) return "";
  if (days == 0) return plugin->text("today");
  if (days > 0 && days <= 6) {
    const std::string soon = tessera::days_text(days);
    if (!soon.empty()) return soon;
  }
  return tessera::date_text(start);
}

// ---- What the entity and the script say ----

enum class Status : uint8_t { CHOOSE, AWAY, NONE, SHOWN };

// A state that holds no recipe: Home Assistant's own words, and the "-" the package's select starts with.
static bool blank(const char *s) {
  return !*s || !strcmp(s, "unknown") || !strcmp(s, "unavailable") || !strcmp(s, "none") || !strcmp(s, "-");
}

// The meal of a tile's data (docs/FIRMWARE_API.md, "What on_state gets"): a Mealie calendar's next event, or a meal
// sensor of the plugin's package, both with `message`, `description` and `start_time`; or a select entity, whose
// state is the recipe chosen by hand.
static Status read_meal(JsonObjectConst data, const char *entity, Meal &m) {
  const char *wait = data["wait"] | "";
  JsonVariantConst state = data["state"];
  if (state.isNull() || strcmp(wait, "wrong_entity") == 0) return Status::CHOOSE;
  const char *s = state | "";
  if (!strcmp(s, "unavailable") || !strcmp(s, "unknown")) return Status::AWAY;
  JsonObjectConst a = data["attributes"];
  const char *name = a["message"] | "";
  if (is_select(entity)) name = blank(s) ? "" : s;
  // A sensor's state is the meal itself when it carries no message.
  else if (!*name && starts(entity, "sensor.") && !blank(s) && strcmp(s, "off")) name = s;
  put(m.name, sizeof m.name, name);
  put(m.description, sizeof m.description, a["description"] | "");
  // The day: as seconds since 1970 (the app turns a `_time` attribute into one), or still as text.
  JsonVariantConst start = a["start_time"];
  m.start = start.is<uint32_t>() ? start.as<uint32_t>() : 0;
  if (!m.start && start.is<const char *>()) {
    const int32_t day = day_of(start.as<const char *>());
    if (day != INT32_MIN) m.start = static_cast<uint32_t>(day) * 86400u + 43200u;
  }
  date_of(m.start, m.date, sizeof m.date);
  put(m.entity, sizeof m.entity, entity);
  return m.name[0] ? Status::SHOWN : Status::NONE;
}

// The recipes a select entity offers, from its `options`.
static void read_choices(JsonObjectConst data, tessera::List<Line> &out) {
  JsonArrayConst options = data["attributes"]["options"].as<JsonArrayConst>();
  if (options.isNull()) return;
  out.clear();
  for (JsonVariantConst option : options) {
    if (static_cast<int>(out.size()) >= MAX_CHOICES) break;
    Line line;
    put(line.text, sizeof line.text, option | "");
    if (!blank(line.text)) out.push_back(line);
  }
}

// The picture's address: the image entity chosen in the inspector, through Home Assistant's own image proxy, marked
// with the moment Home Assistant last heard from it, so a new picture is a new address.
static void picture_of(JsonObjectConst data, const char *image, Meal &m) {
  m.picture[0] = 0;
  if (!image[0]) return;
  JsonObjectConst e = data["entities"]["picture"];
  if (e.isNull()) return;
  const char *state = e["state"] | "";
  if (!*state || !strcmp(state, "unavailable") || !strcmp(state, "unknown")) return;
  const uint32_t mark = e["updated"] | 0u;
  snprintf(m.picture, sizeof m.picture, "/api/image_proxy/%s?v=%lu", image, static_cast<unsigned long>(mark));
}

// The script's facts about a recipe.
static void read_info(JsonObjectConst r, Info &i) {
  i.ok = true;
  put(i.description, sizeof i.description, r["description"] | "");
  i.servings = r["servings"] | 0;
  put(i.yield, sizeof i.yield, r["yield"] | "");
  put(i.total, sizeof i.total, r["total"] | "");
  put(i.prep, sizeof i.prep, r["prep"] | "");
  put(i.cook, sizeof i.cook, r["cook"] | "");
  i.ingredients = r["ingredients"] | 0;
  i.steps = r["steps"] | 0;
}

static std::string dot() { return ui::covers(" · ", Font::BODY) ? " · " : ", "; }

// The description: the entity's own (a calendar's), else the script's.
static const char *description_of(const Meal &m) { return m.description[0] ? m.description : m.info.description; }

// "4 personer" (Mealie's own word), "4 servings", or "".
static std::string servings_text(const tessera::Plugin *plugin, const Info &i) {
  if (i.yield[0]) return i.servings > 0 ? std::to_string(i.servings) + " " + i.yield : std::string(i.yield);
  return i.servings > 0 ? tessera::format(plugin->text("servings"), i.servings) : "";
}

// The facts in one line: the servings and the time it takes.
static std::string facts_text(const tessera::Plugin *plugin, const Info &i) {
  if (!i.ok) return "";
  std::string line = servings_text(plugin, i);
  const char *time = i.total[0] ? i.total : i.cook[0] ? i.cook : i.prep;
  if (*time) line += (line.empty() ? "" : dot()) + time;
  return line;
}

// Whether the answer says the script is not there (not installed, not allowed) rather than the recipe.
static const char *why_not(JsonObjectConst message) {
  JsonObjectConst r = message["result"];
  if ((message["ok"] | false) && !r.isNull()) {
    const char *error = r["error"] | "";
    return !strcmp(error, "not_found") ? "not_found" : "no_script";
  }
  return "no_script";
}

// ---- The tile: the meal of its calendar, or the recipe chosen on its select, with its picture ----
class MealTile : public tessera::Tile, public Asker {
 public:
  MealTile(Mealie *plugin, bool pick) : plugin_(plugin), pick_(pick) {}
  ~MealTile() override { plugin_->forget(this); }

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    tile_ = c.tile;
    put(entity_, sizeof entity_, c.entity);
    put(name_, sizeof name_, c.name);
    put(image_, sizeof image_, c.options["picture"] | "");
    details_ = c.options["details"] | true;
    describe_ = c.options["description"] | true;
    one_ = c.columns == 1 && c.rows == 1;
    const bool pictures = ui::pictures() && image_[0] && !one_;
    const int gap = ui::px(6);
    const int th = ui::line_height(Font::TITLE), bh = ui::line_height(Font::BODY);

    // Where the words go: beside the picture on a flat tile, under it on any other, everywhere without one.
    int x = 0, y = 0, w = width_, h = height_;
    if (pictures) {
      place_ = ui::picture(c.parent);
      if (c.rows == 1) {
        const int side = std::min(height_, width_ / 2);
        lv_obj_set_size(place_, side, height_);
        lv_obj_set_pos(place_, 0, 0);
        x = side + gap;
        w = width_ - x;
      } else {
        const int ph = std::min(height_ * 45 / 100, width_);
        lv_obj_set_size(place_, width_, ph);
        lv_obj_set_pos(place_, 0, 0);
        y = ph + gap;
        h = height_ - y;
      }
    }

    // The head: the meal ("Dinner") and, at the right, when it is; on one cell, when it is on a line of its own.
    head_ = ui::label(c.parent, Font::TITLE, theme::INK);
    lv_obj_set_pos(head_, x, y);
    when_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    int body_top = y + th, room = h - th;
    if (one_) {
      lv_obj_set_width(head_, w);
      lv_obj_set_size(when_, w, bh);
      lv_obj_set_pos(when_, x, y + h - bh);
      room -= bh;
    } else {
      const int ww = w * 45 / 100;
      lv_obj_set_size(when_, ww, bh);
      lv_obj_set_style_text_align(when_, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_pos(when_, x + w - ww, y + (th - bh) / 2);
      lv_obj_set_width(head_, std::max(1, w - ww - gap));
    }

    // The name in the largest face whose two lines leave room for the facts under them, the facts at the bottom, the
    // description between them where lines remain.
    Font face = Font::TITLE;
    if (!one_ && w >= ui::px(150) && 2 * ui::line_height(Font::HEADLINE) + bh <= room) face = Font::HEADLINE;
    const int lh = ui::line_height(face);
    if (!one_ && room >= 2 * lh + bh) {
      facts_ = ui::label(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_size(facts_, w, bh);
      lv_obj_set_pos(facts_, x, y + h - bh);
      room -= bh;
    }
    const int lines = std::max(1, std::min(describe_ ? 2 : 3, room / lh));
    title_ = ui::text(c.parent, face, theme::INK);
    lv_obj_set_size(title_, w, lines * lh);
    lv_obj_set_pos(title_, x, body_top);
    const int rest = room - lines * lh;
    if (describe_ && rest >= bh) {
      desc_ = ui::text(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_size(desc_, w, std::min(3, rest / bh) * bh);
      lv_obj_set_pos(desc_, x, body_top + lines * lh);
    }
    // What the tile says while it has no meal: in the middle of the words' room.
    note_ = ui::text(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(note_, w, 2 * bh);
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(note_, x, body_top + std::max(0, (room - 2 * bh) / 2));
    put(word_, sizeof word_, plugin_->meal_word(entity_).c_str());
  }

  void on_state(JsonObjectConst data) override {
    Meal m;
    status_ = read_meal(data, entity_, m);
    picture_of(data, image_, m);
    if (pick_) read_choices(data, choices_);
    // Another meal, or another day: the facts are asked again; the same one keeps them.
    if (strcmp(m.name, meal_.name) != 0 || strcmp(m.date, meal_.date) != 0) {
      asking_ = false;
      failed_at_ = 0;
    } else {
      m.info = meal_.info;
    }
    meal_ = m;
    ask();
    texts();
  }

  void on_tick(uint32_t epoch) override {
    // The day words follow the clock: "Tomorrow" becomes "Today" at midnight.
    if (epoch / 60 != minute_) {
      minute_ = epoch / 60;
      texts();
    }
    ask();
  }

  // The script's facts came, or did not.
  void answer(JsonObjectConst message) override {
    asking_ = false;
    JsonObjectConst r = message["result"];
    if ((message["ok"] | false) && (r["ok"] | false)) {
      read_info(r, meal_.info);
    } else {
      failed_at_ = esphome::millis() | 1u;
    }
    texts();
  }

  void on_theme() override {
    ui::set_color(head_, theme::INK);
    ui::set_color(when_, theme::MUTED);
    ui::set_color(title_, theme::INK);
    if (desc_) ui::set_color(desc_, theme::MUTED);
    if (facts_) ui::set_color(facts_, theme::MUTED);
    ui::set_color(note_, theme::MUTED);
  }

  // A tap opens the recipe; on a select tile, the list to choose from.
  void on_tap() override {
    if (status_ == Status::CHOOSE) return;
    plugin_->current = meal_;
    if (pick_) {
      plugin_->choices = choices_;
      tessera::open_card(plugin_->plugin_id(), "choose", entity_, tile_, name_[0] ? name_ : word_);
      return;
    }
    tessera::open_card(plugin_->plugin_id(), "recipe", entity_, tile_, meal_.name);
  }

 private:
  // The facts of the meal, once per meal, and again after a while when the script did not answer.
  void ask() {
    if (!details_ || status_ != Status::SHOWN || meal_.info.ok || asking_) return;
    if (failed_at_ && esphome::millis() - failed_at_ < RETRY_MS) return;
    asking_ = plugin_->ask(this, meal_, "info", 0, 0, 0) != 0;
    if (!asking_) failed_at_ = esphome::millis() | 1u;
  }

  void texts() {
    const bool shown = status_ == Status::SHOWN;
    ui::set_text(head_, name_[0] ? name_ : word_);
    ui::set_text(when_, shown ? when_text(plugin_, meal_.start) : "");
    ui::set_text(title_, shown ? meal_.name : "");
    if (desc_) ui::set_text(desc_, shown ? description_of(meal_) : "");
    if (facts_) ui::set_text(facts_, shown ? facts_text(plugin_, meal_.info) : "");
    const char *why = status_ == Status::CHOOSE ? (pick_ ? "choose_select" : "choose")
                    : status_ == Status::AWAY ? "away"
                    : status_ == Status::NONE ? (pick_ ? "none_chosen" : "none") : nullptr;
    ui::set_text(note_, why ? plugin_->text(why) : "");
    if (place_) ui::set_picture(place_, shown ? meal_.picture : "", tessera::Fit::COVER);
  }

  Mealie *plugin_;
  bool pick_;
  int width_ = 0, height_ = 0, tile_ = -1;
  char entity_[64] = {}, name_[52] = {}, image_[64] = {}, word_[32] = {};
  bool details_ = true, describe_ = true, one_ = false, asking_ = false;
  uint32_t failed_at_ = 0, minute_ = 0;
  Status status_ = Status::CHOOSE;
  Meal meal_;
  tessera::List<Line> choices_;
  lv_obj_t *place_{}, *head_{}, *when_{}, *title_{}, *desc_{}, *facts_{}, *note_{};
};

// ---- The card: the recipe, its ingredients and its steps page by page ----
class RecipeCard : public tessera::Card, public Asker {
 public:
  explicit RecipeCard(Mealie *plugin) : plugin_(plugin) {}
  ~RecipeCard() override { plugin_->forget(this); }

  void open(const tessera::CardContext &c) override {
    width_ = c.width;
    height_ = c.height;
    meal_ = plugin_->current;
    // Opened for another entity than the tile tapped last (a settings row, the bench): nothing is known of it yet.
    if (c.entity && *c.entity && strcmp(meal_.entity, c.entity) != 0) {
      meal_ = Meal();
      put(meal_.entity, sizeof meal_.entity, c.entity);
    }
    const int gap = ui::px(8), bh = ui::line_height(Font::BODY), hh = ui::line_height(Font::HEADLINE);
    const int lh = ui::line_height(Font::BODY_LARGE), th = ui::line_height(Font::TITLE);

    // The head: the picture at the left, the name and the facts beside it, the description under them.
    int x = 0, w = width_, y = 2 * hh + bh;
    const bool pictures = ui::pictures() && meal_.picture[0];
    if (pictures) {
      const int side = std::min(width_ * 35 / 100, std::max(2 * hh + bh, ui::mm(16)));
      place_ = ui::picture(c.parent);
      lv_obj_set_size(place_, side, side);
      lv_obj_set_pos(place_, 0, 0);
      ui::set_picture(place_, meal_.picture, tessera::Fit::COVER);
      x = side + gap;
      w = width_ - x;
      y = std::max(y, side);
    }
    name_ = ui::text(c.parent, Font::HEADLINE, theme::INK);
    lv_obj_set_size(name_, w, 2 * hh);
    lv_obj_set_pos(name_, x, 0);
    facts_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(facts_, w, bh);
    lv_obj_set_pos(facts_, x, 2 * hh);
    y += gap;
    if ((meal_.description[0] || is_select(meal_.entity)) && height_ - y > 6 * lh) {
      desc_ = ui::text(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_size(desc_, width_, 2 * bh);
      lv_obj_set_pos(desc_, 0, y);
      y += 2 * bh + gap;
    }
    header_ = ui::label(c.parent, Font::TITLE, theme::INK);
    lv_obj_set_size(header_, width_, th);
    lv_obj_set_pos(header_, 0, y);
    y += th + ui::px(2);

    // The lines: as many rows as fit above the page count, each one line of the key's face.
    indent_ = ui::text_width("8. ", Font::BODY_LARGE);
    const int row = lh + ui::px(2);
    const int count = std::max(1, std::min(MAX_LINES, (height_ - y - bh - gap) / row));
    for (int i = 0; i < count; ++i) {
      Row r;
      r.label = ui::label(c.parent, Font::BODY_LARGE, theme::INK);
      lv_obj_set_size(r.label, width_, lh);
      lv_obj_set_pos(r.label, 0, y + i * row);
      rows_.push_back(r);
    }
    // How many characters a line may hold: the script wraps its words to that.
    const int cw = std::max(1, ui::text_width("abcdefghijklmnopqrstuvwxyz", Font::BODY_LARGE) / 26);
    cols_ = std::max(8, std::min(MAX_COLS, (width_ - indent_) / cw));
    per_ = count;
    note_ = ui::text(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(note_, width_, 3 * bh);
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(note_, 0, y + ui::px(4));
    hint_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(hint_, width_ / 2, bh);
    lv_obj_set_pos(hint_, 0, height_ - bh);
    pager_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(pager_, width_ - width_ / 2, bh);
    lv_obj_set_style_text_align(pager_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(pager_, width_ / 2, height_ - bh);
    texts();
    ask(false, 1);
  }

  // The tile it was opened from changed: another meal starts the recipe again. A recipe chosen on the picker stays
  // while Home Assistant catches up with the choice.
  void on_state(JsonObjectConst data) override {
    if (meal_.fixed) return;
    Meal m;
    if (read_meal(data, meal_.entity, m) != Status::SHOWN || !strcmp(m.name, meal_.name)) return;
    put(meal_.name, sizeof meal_.name, m.name);
    put(meal_.description, sizeof meal_.description, m.description);
    put(meal_.date, sizeof meal_.date, m.date);
    meal_.start = m.start;
    meal_.info = Info();
    lines_ = 0;
    texts();
    ask(false, 1);
  }

  // A page of lines, or the reason there is none.
  void answer(JsonObjectConst message) override {
    asking_ = false;
    JsonObjectConst r = message["result"];
    if (!(message["ok"] | false) || !(r["ok"] | false)) {
      why_ = why_not(message);
      lines_ = 0;
      texts();
      return;
    }
    why_ = nullptr;
    if (!r["name"].isNull()) read_info(r, meal_.info);
    JsonArrayConst lines = r["lines"].as<JsonArrayConst>();
    if (!lines.isNull()) {
      steps_ = !strcmp(r["part"] | "ingredients", "steps");
      page_ = std::max(1, r["page"] | 1);
      pages_ = std::max(1, r["pages"] | 1);
      lines_ = 0;
      for (JsonVariantConst line : lines) {
        if (lines_ >= static_cast<int>(rows_.size())) break;
        put(rows_[lines_].text, sizeof rows_[lines_].text, line | "");
        ++lines_;
      }
    }
    texts();
  }

  // A tap turns the page: the next one, then the steps after the ingredients, then the ingredients again.
  void on_tap() override {
    if (asking_) return;
    if (page_ < pages_) ask(steps_, page_ + 1);
    else ask(!steps_, 1);
  }

  void on_theme() override {
    ui::set_color(name_, theme::INK);
    ui::set_color(facts_, theme::MUTED);
    if (desc_) ui::set_color(desc_, theme::MUTED);
    ui::set_color(header_, theme::INK);
    for (auto &r : rows_) ui::set_color(r.label, theme::INK);
    ui::set_color(note_, theme::MUTED);
    ui::set_color(hint_, theme::MUTED);
    ui::set_color(pager_, theme::MUTED);
  }

 private:
  struct Row {
    char text[52] = {};
    lv_obj_t *label = nullptr;
  };

  void ask(bool steps, int page) {
    if (!meal_.name[0]) {
      why_ = "none";
      texts();
      return;
    }
    asking_ = plugin_->ask(this, meal_, steps ? "steps" : "ingredients", page, per_, cols_) != 0;
    if (!asking_) why_ = "no_script";
    texts();
  }

  void texts() {
    ui::set_text(name_, meal_.name);
    std::string facts = when_text(plugin_, meal_.start);
    const std::string more = facts_text(plugin_, meal_.info);
    if (!more.empty()) facts += (facts.empty() ? "" : dot()) + more;
    ui::set_text(facts_, facts);
    if (desc_) ui::set_text(desc_, description_of(meal_));
    ui::set_text(header_, plugin_->text(steps_ ? "steps" : "ingredients"));
    // Each line: a section's name in the title's face, a line that goes on indented under the one before it.
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
      const char *line = i < lines_ ? rows_[i].text : "";
      const bool section = line[0] == '#' && line[1] == ' ';
      const bool more_of = line[0] == ' ';
      const Font face = section ? Font::TITLE : Font::BODY_LARGE;
      ui::set_font(rows_[i].label, face);
      lv_obj_set_height(rows_[i].label, ui::line_height(face));
      lv_obj_set_x(rows_[i].label, more_of ? indent_ : 0);
      ui::set_text(rows_[i].label, section ? line + 2 : more_of ? line + 1 : line);
    }
    ui::set_text(note_, why_ ? plugin_->text(why_) : asking_ && !lines_ ? plugin_->text("asking") : "");
    const bool pages = !why_ && lines_ > 0;
    ui::set_text(hint_, pages ? plugin_->text("tap_more") : "");
    ui::set_text(pager_, pages ? tessera::fill(tessera::fill(plugin_->text("page"), "n", std::to_string(page_)).c_str(),
                                               "m", std::to_string(pages_))
                               : "");
  }

  Mealie *plugin_;
  int width_ = 0, height_ = 0, indent_ = 0, cols_ = MAX_COLS, per_ = 1;
  int page_ = 1, pages_ = 1, lines_ = 0;
  bool steps_ = false, asking_ = false;
  const char *why_ = nullptr;   // a text key for why there are no lines, or none
  Meal meal_;
  tessera::List<Row> rows_;
  lv_obj_t *place_{}, *name_{}, *facts_{}, *desc_{}, *header_{}, *note_{}, *hint_{}, *pager_{};
};

// ---- The picker: the recipes of a select entity, a tap chooses one and opens its recipe ----
class ChooseCard : public tessera::Card {
 public:
  explicit ChooseCard(Mealie *plugin) : plugin_(plugin) {}

  void open(const tessera::CardContext &c) override {
    width_ = c.width;
    height_ = c.height;
    tile_ = c.tile;
    put(entity_, sizeof entity_, c.entity ? c.entity : plugin_->current.entity);
    put(current_, sizeof current_, plugin_->current.name);
    choices_ = plugin_->choices;
    const int gap = ui::px(8), bh = ui::line_height(Font::BODY), lh = ui::line_height(Font::BODY_LARGE);
    // A row a finger can hit: a dot for the one chosen, the recipe's name beside it.
    dot_ = ui::px(ui::large() ? 10 : 8);
    row_h_ = std::max(lh + ui::px(6), ui::mm(7));
    count_ = std::max(1, std::min(MAX_CHOICES, (height_ - bh - gap) / row_h_));
    for (int i = 0; i < count_; ++i) {
      Row r;
      r.mark = ui::block(c.parent, theme::ACCENT);
      lv_obj_set_size(r.mark, dot_, dot_);
      lv_obj_set_style_radius(r.mark, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_pos(r.mark, 0, i * row_h_ + (row_h_ - dot_) / 2);
      r.label = ui::label(c.parent, Font::BODY_LARGE, theme::INK);
      lv_obj_set_size(r.label, std::max(1, width_ - dot_ - gap), lh);
      lv_obj_set_pos(r.label, dot_ + gap, i * row_h_ + (row_h_ - lh) / 2);
      rows_.push_back(r);
    }
    note_ = ui::text(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(note_, width_, 2 * bh);
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(note_, 0, ui::px(4));
    hint_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(hint_, width_ / 2, bh);
    lv_obj_set_pos(hint_, 0, height_ - bh);
    pager_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(pager_, width_ - width_ / 2, bh);
    lv_obj_set_style_text_align(pager_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(pager_, width_ / 2, height_ - bh);
    texts();
  }

  // The tile's data: the recipe chosen now, and the list; Home Assistant reports the choice a moment after a tap.
  void on_state(JsonObjectConst data) override {
    read_choices(data, choices_);
    JsonVariantConst state = data["state"];
    if (!state.isNull()) {
      const char *s = state | "";
      put(current_, sizeof current_, blank(s) ? "" : s);
    }
    texts();
  }

  // A tap on a row chooses that recipe and opens it; a tap on the line at the bottom turns the page.
  void on_tap() override {
    const tessera::Point p = tessera::tap_point();
    if (p.y < 0) return;
    if (p.y >= count_ * row_h_) {
      if (pages() > 1) {
        page_ = page_ % pages() + 1;
        texts();
      }
      return;
    }
    const int at = (page_ - 1) * count_ + p.y / row_h_;
    if (at < 0 || at >= static_cast<int>(choices_.size())) return;
    choose(choices_[at].text);
  }

  void on_theme() override {
    for (auto &r : rows_) {
      ui::set_color(r.label, theme::INK);
      ui::set_fill(r.mark, Tone::ACCENT, theme::ACCENT);
    }
    ui::set_color(note_, theme::MUTED);
    ui::set_color(hint_, theme::MUTED);
    ui::set_color(pager_, theme::MUTED);
  }

 private:
  struct Row {
    lv_obj_t *mark = nullptr, *label = nullptr;
  };

  int pages() const { return std::max(1, (static_cast<int>(choices_.size()) + count_ - 1) / count_); }

  // Tell Home Assistant, then show the recipe at once: its card keeps the choice while the select catches up.
  void choose(const char *name) {
    Meal &m = plugin_->current;
    if (strcmp(name, current_) != 0) {
      JsonDocument fields;
      fields["option"] = name;
      const char *service = starts(entity_, "select.") ? "select.select_option" : "input_select.select_option";
      tessera::action(service, entity_, fields.as<JsonVariantConst>());
      put(m.name, sizeof m.name, name);
      m.description[0] = 0;
      m.picture[0] = 0;
      m.info = Info();
    }
    put(m.entity, sizeof m.entity, entity_);
    m.date[0] = 0;
    m.start = 0;
    m.fixed = true;
    tessera::open_card(plugin_->plugin_id(), "recipe", entity_, tile_, name);
  }

  void texts() {
    const int n = static_cast<int>(choices_.size());
    if (page_ > pages()) page_ = 1;
    for (int i = 0; i < count_; ++i) {
      const int at = (page_ - 1) * count_ + i;
      const bool there = at < n;
      ui::set_text(rows_[i].label, there ? choices_[at].text : "");
      if (there && !strcmp(choices_[at].text, current_)) lv_obj_remove_flag(rows_[i].mark, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(rows_[i].mark, LV_OBJ_FLAG_HIDDEN);
    }
    ui::set_text(note_, n ? "" : plugin_->text("no_choices"));
    ui::set_text(hint_, n ? plugin_->text(pages() > 1 ? "tap_more" : "choose_tap") : "");
    ui::set_text(pager_, pages() > 1 ? tessera::fill(tessera::fill(plugin_->text("page"), "n", std::to_string(page_)).c_str(),
                                                     "m", std::to_string(pages()))
                                     : "");
  }

  Mealie *plugin_;
  int width_ = 0, height_ = 0, tile_ = -1, dot_ = 0, row_h_ = 1, count_ = 1, page_ = 1;
  char entity_[64] = {}, current_[52] = {};
  tessera::List<Line> choices_;
  tessera::List<Row> rows_;
  lv_obj_t *note_{}, *hint_{}, *pager_{};
};

// ---- The plugin ----

void Mealie::setup() {
  add_tile("meal", [this]() { return new MealTile(this, false); });
  add_tile("pick", [this]() { return new MealTile(this, true); });
  add_card("recipe", [this]() { return new RecipeCard(this); });
  add_card("choose", [this]() { return new ChooseCard(this); });
  add_bar_item("today", [this]() { return bar(); });
}

uint32_t Mealie::ask(Asker *who, const Meal &meal, const char *part, int page, int per, int cols) {
  JsonDocument request;
  request["ask"] = SCRIPT;
  JsonObject data = request["data"].to<JsonObject>();
  data["recipe"] = meal.name;
  if (meal.date[0]) data["date"] = meal.date;
  data["part"] = part;
  if (page > 0) {
    data["page"] = page;
    data["per"] = per;
    data["cols"] = cols;
  }
  const uint32_t re = tessera::send(this, request.as<JsonObjectConst>());
  if (re) asked_.push_back({re, who});
  return re;
}

void Mealie::forget(Asker *who) {
  asked_.erase(std::remove_if(asked_.begin(), asked_.end(), [who](const std::pair<uint32_t, Asker *> &a) {
                 return a.second == who;
               }),
               asked_.end());
}

void Mealie::on_message(JsonObjectConst message) {
  const uint32_t re = message["re"] | 0u;
  for (auto it = asked_.begin(); it != asked_.end(); ++it) {
    if (it->first != re) continue;
    Asker *who = it->second;
    asked_.erase(it);
    who->answer(message);
    return;
  }
}

std::string Mealie::meal_word(const char *entity) const {
  if (is_select(entity)) return text("chosen");
  static const char *const KINDS[] = {"breakfast", "lunch", "dinner", "side", "dessert", "snack", "drink"};
  for (const char *kind : KINDS)
    if (entity && strstr(entity, kind)) return text(kind);
  return text("meal");
}

// "Dinner: Vodkapasta" in the top bar on the day of the meal.
tessera::BarItem Mealie::bar() const {
  tessera::BarItem item;
  if (!message_ || !start_ || message_->state.empty()) return item;
  const int32_t day = day_of(start_->state.c_str()), now = today();
  if (day == INT32_MIN || now == INT32_MIN || day != now) return item;
  item.shown = true;
  item.icon = 0xF0A70;  // silverware-fork-knife
  item.text = tessera::fill(tessera::fill(text("bar"), "meal", meal_word(meal_.c_str())).c_str(), "what", message_->state);
  return item;
}

}  // namespace esphome::mealie
