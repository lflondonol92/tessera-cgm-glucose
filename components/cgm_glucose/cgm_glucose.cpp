#include "cgm_glucose.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace esphome::cgm_glucose {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

// How many mg/dL one mmol/L is. A sensor in mmol/L is converted before it meets the limits, which are in mg/dL.
static constexpr float MGDL_PER_MMOL = 18.0182f;
// A change is worked out by the tile only between two readings closer together than this (about two polls).
static constexpr uint32_t MAX_DELTA_GAP_S = 9 * 60;

// The seven arrows, as the trend of every integration is brought to one set. The words and codes that name each one,
// and the rate thresholds below, follow SugarTV Card (MIT, Victor Kuznetsov), see NOTICE.
enum class Trend : uint8_t {
  UNKNOWN,
  RISING_QUICKLY,
  RISING,
  RISING_SLIGHTLY,
  STEADY,
  FALLING_SLIGHTLY,
  FALLING,
  FALLING_QUICKLY
};

// How the trend sensor's state is read.
enum class TrendFormat : uint8_t { TEXT, CARELINK, RATE };

// Where a reading sits against the limits. Not named LOW and HIGH: the Arduino framework defines those as macros.
enum class Zone : uint8_t { IN_RANGE, BELOW, URGENT_BELOW, ABOVE, URGENT_ABOVE };

struct TrendWord {
  const char *word;
  Trend trend;
};

// Lower case, letters, digits, spaces and underscores only (see normalize_trend).
static const TrendWord TREND_WORDS[] = {
    // Dexcom, already in this set
    {"rising_quickly", Trend::RISING_QUICKLY},
    {"rising", Trend::RISING},
    {"rising_slightly", Trend::RISING_SLIGHTLY},
    {"steady", Trend::STEADY},
    {"falling_slightly", Trend::FALLING_SLIGHTLY},
    {"falling", Trend::FALLING},
    {"falling_quickly", Trend::FALLING_QUICKLY},
    // Nightscout directions, and the snake_case states of Nightscout Extended
    {"doubleup", Trend::RISING_QUICKLY},
    {"double_up", Trend::RISING_QUICKLY},
    {"singleup", Trend::RISING},
    {"single_up", Trend::RISING},
    {"fortyfiveup", Trend::RISING_SLIGHTLY},
    {"forty_five_up", Trend::RISING_SLIGHTLY},
    {"flat", Trend::STEADY},
    {"fortyfivedown", Trend::FALLING_SLIGHTLY},
    {"forty_five_down", Trend::FALLING_SLIGHTLY},
    {"singledown", Trend::FALLING},
    {"single_down", Trend::FALLING},
    {"doubledown", Trend::FALLING_QUICKLY},
    {"double_down", Trend::FALLING_QUICKLY},
    // LibreView numbers
    {"2", Trend::RISING_QUICKLY},
    {"3", Trend::RISING},
    {"4", Trend::RISING_SLIGHTLY},
    {"5", Trend::STEADY},
    {"6", Trend::FALLING_SLIGHTLY},
    {"7", Trend::FALLING},
    {"8", Trend::FALLING_QUICKLY},
    // LibreView (PTST) and Medtrum EasyView
    {"rising quickly", Trend::RISING_QUICKLY},
    {"rising slightly", Trend::RISING_SLIGHTLY},
    {"stable", Trend::STEADY},
    {"falling slightly", Trend::FALLING_SLIGHTLY},
    {"falling quickly", Trend::FALLING_QUICKLY},
    {"decreasing_very_fast", Trend::FALLING_QUICKLY},
    {"decreasing_fast", Trend::FALLING_QUICKLY},
    {"decreasing", Trend::FALLING},
    {"increasing", Trend::RISING},
    {"increasing_fast", Trend::RISING_QUICKLY},
    {"increasing_very_fast", Trend::RISING_QUICKLY},
    // LibreLink (gillesvs, dodog), Sugar Sense, LibreLinkUp
    {"decreasing fast", Trend::FALLING_QUICKLY},
    {"increasing fast", Trend::RISING_QUICKLY},
    {"rising fast", Trend::RISING_QUICKLY},
    {"rising_fast", Trend::RISING_QUICKLY},
    {"falling fast", Trend::FALLING_QUICKLY},
    {"falling_fast", Trend::FALLING_QUICKLY},
    {"rising_rapidly", Trend::RISING_QUICKLY},
    {"falling_rapidly", Trend::FALLING_QUICKLY},
    // Carelink (Medtronic): UP, DOWN, FLAT and the doubles
    {"up", Trend::RISING},
    {"up_up", Trend::RISING_QUICKLY},
    {"up_double", Trend::RISING_QUICKLY},
    {"down", Trend::FALLING},
    {"down_down", Trend::FALLING_QUICKLY},
    {"down_double", Trend::FALLING_QUICKLY},
};

// A trend as text, brought to one of the seven arrows. Anything it does not know is UNKNOWN: a direction written with
// a glyph after it ("FortyFiveUp ↗") still names a trend, so every character that is not a letter, a digit, a space or
// an underscore is dropped first. NONE means "no arrow" for most integrations and "too slow for an arrow" for Carelink.
static Trend normalize_trend(const char *raw, bool carelink) {
  if (raw == nullptr) return Trend::UNKNOWN;
  char key[40];
  size_t n = 0;
  for (const char *p = raw; *p != '\0' && n < sizeof(key) - 1; ++p) {
    unsigned char ch = static_cast<unsigned char>(*p);
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<unsigned char>(ch + 32);
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == ' ') key[n++] = static_cast<char>(ch);
  }
  key[n] = '\0';
  char *start = key;
  while (*start == ' ') ++start;
  size_t len = strlen(start);
  while (len > 0 && start[len - 1] == ' ') start[--len] = '\0';
  if (len == 0) return Trend::UNKNOWN;
  if (strcmp(start, "none") == 0) return carelink ? Trend::STEADY : Trend::UNKNOWN;
  for (const TrendWord &w : TREND_WORDS) {
    if (strcmp(start, w.word) == 0) return w.trend;
  }
  return Trend::UNKNOWN;
}

// A sensor's state as a number. The two states Home Assistant uses for "no value" are not numbers, and a decimal comma
// (a template sensor in a language that writes it) is read as a point.
static bool parse_number(const char *state, float &out) {
  if (state == nullptr || *state == '\0') return false;
  if (strcmp(state, "unknown") == 0 || strcmp(state, "unavailable") == 0) return false;
  char buf[24];
  size_t n = 0;
  for (const char *p = state; *p != '\0' && n < sizeof(buf) - 1; ++p) buf[n++] = (*p == ',') ? '.' : *p;
  buf[n] = '\0';
  char *end = nullptr;
  const float v = strtof(buf, &end);
  if (end == buf || !std::isfinite(v)) return false;
  while (*end == ' ') ++end;
  if (*end != '\0') return false;
  out = v;
  return true;
}

// A trend sensor that reports a rate (mg/dL or mmol/L per minute): the thresholds Dexcom and Tandem use, 1, 2 and
// 3 mg/dL per minute.
static Trend trend_from_rate(const char *state, const char *unit) {
  float rate = 0;
  if (!parse_number(state, rate)) return Trend::UNKNOWN;
  if (strstr(unit, "mmol") != nullptr) rate *= MGDL_PER_MMOL;
  const float a = fabsf(rate);
  const bool up = rate > 0;
  if (a < 1) return Trend::STEADY;
  if (a < 2) return up ? Trend::RISING_SLIGHTLY : Trend::FALLING_SLIGHTLY;
  if (a <= 3) return up ? Trend::RISING : Trend::FALLING;
  return up ? Trend::RISING_QUICKLY : Trend::FALLING_QUICKLY;
}

// Days since 1970-01-01 for a date (Howard Hinnant's days_from_civil).
static long days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long>(doe) - 719468;
}

// Epoch values come in seconds from some integrations and milliseconds from others; anything below this bound is too
// small to be a modern millisecond value, so it is seconds.
static constexpr double EPOCH_SECONDS_BOUND = 1e11;

static uint32_t epoch_from_number(double v) {
  if (!(v > 0)) return 0;
  if (v >= EPOCH_SECONDS_BOUND) v /= 1000.0;
  return static_cast<uint32_t>(v);
}

// A moment as text: an ISO 8601 date and time ("2026-11-22T17:16:00+00:00", with a Z, an offset or none, which is
// read as UTC) or a number of seconds or milliseconds since 1970. 0 when it is neither.
static uint32_t parse_time_text(const char *s) {
  if (s == nullptr || *s == '\0') return 0;
  char *end = nullptr;
  const double num = strtod(s, &end);
  if (end != s && *end == '\0') return epoch_from_number(num);

  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, used = 0;
  if (sscanf(s, "%d-%d-%d%*[Tt ]%d:%d:%d%n", &y, &mo, &d, &h, &mi, &sec, &used) != 6 || used == 0) return 0;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 60) return 0;

  const char *p = s + used;
  if (*p == '.') {
    ++p;
    while (*p >= '0' && *p <= '9') ++p;
  }
  long offset = 0;
  if (*p == '+' || *p == '-') {
    const long sign = (*p == '-') ? -1 : 1;
    ++p;
    int oh = 0, om = 0;
    const bool colon = p[0] != '\0' && p[1] != '\0' && p[2] == ':';
    if (sscanf(p, colon ? "%2d:%2d" : "%2d%2d", &oh, &om) < 1) return 0;
    offset = sign * (oh * 3600L + om * 60L);
  } else if (*p != '\0' && *p != 'Z' && *p != 'z') {
    return 0;
  }
  const long t = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400L + h * 3600L +
                 mi * 60L + sec - offset;
  return t > 0 ? static_cast<uint32_t>(t) : 0;
}

// A moment that came as text or as a number (the app turns attributes named ..._at, ..._time or ...date into seconds).
static uint32_t epoch_of(JsonVariantConst v) {
  if (v.is<const char *>()) return parse_time_text(v.as<const char *>());
  if (v.is<double>() || v.is<long>() || v.is<unsigned long>()) return epoch_from_number(v.as<double>());
  return 0;
}

// An attribute that may be a word or a number (LibreView's trend is a number), as text.
static const char *attribute_text(JsonVariantConst v, char *buf, size_t size) {
  if (v.is<const char *>()) return v.as<const char *>();
  if (v.is<long>()) {
    snprintf(buf, size, "%ld", v.as<long>());
    return buf;
  }
  return "";
}

static bool is_mmol(const char *unit) {
  if (unit == nullptr) return false;
  char low[12];
  size_t n = 0;
  for (const char *p = unit; *p != '\0' && n < sizeof(low) - 1; ++p) {
    char ch = *p;
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
    low[n++] = ch;
  }
  low[n] = '\0';
  return strcmp(low, "mmol/l") == 0;
}

// The tile: the reading in the middle, the trend arrow beside it, then how old it is, the change and the active insulin
// as far as the height holds them. on_state() reads the entities the person chose; on_tick() moves the age on.
class GlucoseTile : public tessera::Tile {
 public:
  explicit GlucoseTile(const tessera::Plugin *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    const char *format = c.options["trend_format"] | "text";
    trend_format_ = strcmp(format, "carelink") == 0 ? TrendFormat::CARELINK
                    : strcmp(format, "rate") == 0   ? TrendFormat::RATE
                                                    : TrendFormat::TEXT;
    limits_[0] = c.options["urgent_low"] | 54.0f;
    limits_[1] = c.options["low"] | 70.0f;
    limits_[2] = c.options["high"] | 180.0f;
    limits_[3] = c.options["urgent_high"] | 250.0f;
    stale_s_ = static_cast<uint32_t>(c.options["stale_after"] | 15) * 60u;

    value_ = ui::label(c.parent, Font::VALUE, theme::INK);
    for (lv_obj_t *&a : arrow_) a = ui::label(c.parent, Font::ICON, theme::INK);
    age_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    change_label_ = ui::label(c.parent, Font::BODY, theme::INK);
    insulin_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    for (lv_obj_t *l : {age_, change_label_, insulin_}) {
      lv_obj_set_width(l, width_);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    paint();
  }

  void on_state(JsonObjectConst data) override {
    JsonObjectConst entities = data["entities"];
    JsonObjectConst g = entities["glucose"];
    chosen_ = !g.isNull();

    valid_ = false;
    if (chosen_) {
      float v = 0;
      valid_ = parse_number(g["state"] | "", v);
      if (valid_) reading_ = v;
      mmol_ = is_mmol(g["attributes"]["unit_of_measurement"] | "");
    }

    trend_ = read_trend(entities, g);
    const uint32_t time = read_time(entities, g);
    read_delta(entities, valid_ ? reading_ : 0.0f, time);
    read_insulin(entities);
    time_ = time;
    paint();
  }

  void on_tick(uint32_t epoch) override {
    epoch_ = epoch;
    paint();
  }

  void on_theme() override { paint(); }

 private:
  // The trend: the trend sensor if one was chosen, else an attribute of the glucose sensor (Nightscout's direction,
  // LibreView's trend).
  Trend read_trend(JsonObjectConst entities, JsonObjectConst g) const {
    JsonObjectConst t = entities["trend"];
    if (!t.isNull()) {
      const char *state = t["state"] | "";
      const char *unit = t["attributes"]["unit_of_measurement"] | "";
      if (trend_format_ == TrendFormat::RATE || (strlen(unit) >= 4 && strcmp(unit + strlen(unit) - 4, "/min") == 0))
        return trend_from_rate(state, unit);
      return normalize_trend(state, trend_format_ == TrendFormat::CARELINK);
    }
    char buf[16];
    JsonObjectConst a = g["attributes"];
    Trend found = normalize_trend(attribute_text(a["direction"], buf, sizeof(buf)), false);
    if (found == Trend::UNKNOWN) found = normalize_trend(attribute_text(a["trend"], buf, sizeof(buf)), false);
    return found;
  }

  // When the reading was taken: the time sensor, else an attribute of the glucose sensor, else the moment Home
  // Assistant last heard from the glucose sensor. 0 when none is known.
  uint32_t read_time(JsonObjectConst entities, JsonObjectConst g) const {
    uint32_t t = 0;
    JsonObjectConst r = entities["reading_time"];
    if (!r.isNull()) t = epoch_of(r["state"]);
    if (t == 0) {
      JsonObjectConst a = g["attributes"];
      t = epoch_of(a["measured_at"]);
      if (t == 0) t = epoch_of(a["measurement_timestamp"]);
      if (t == 0) t = epoch_of(a["date"]);
    }
    if (t == 0) t = g["updated"] | 0u;
    return t;
  }

  // The change since the previous reading: the sensor's own when there is one, else the tile's, from the last two
  // different readings it saw, kept only when they are close enough to be neighbours.
  void read_delta(JsonObjectConst entities, float value, uint32_t time) {
    delta_valid_ = false;
    JsonObjectConst d = entities["delta"];
    if (!d.isNull()) {
      float dv = 0;
      if (parse_number(d["state"] | "", dv)) {
        delta_ = dv;
        delta_valid_ = true;
      }
      return;
    }
    if (!valid_ || time == 0) return;
    if (time != cur_time_) {
      prev_value_ = cur_value_;
      prev_time_ = cur_time_;
      cur_time_ = time;
    }
    cur_value_ = value;
    if (prev_time_ != 0 && cur_time_ > prev_time_ && cur_time_ - prev_time_ < MAX_DELTA_GAP_S) {
      delta_ = cur_value_ - prev_value_;
      delta_valid_ = true;
    }
  }

  void read_insulin(JsonObjectConst entities) {
    insulin_valid_ = false;
    JsonObjectConst i = entities["insulin"];
    if (i.isNull()) return;
    float v = 0;
    if (!parse_number(i["state"] | "", v)) return;
    insulin_value_ = v;
    const char *unit = i["attributes"]["unit_of_measurement"] | "";
    snprintf(insulin_unit_, sizeof(insulin_unit_), "%s", *unit != '\0' ? unit : "U");
    insulin_valid_ = true;
  }

  Zone zone() const {
    const float mgdl = mmol_ ? reading_ * MGDL_PER_MMOL : reading_;
    if (mgdl < limits_[0]) return Zone::URGENT_BELOW;
    if (mgdl < limits_[1]) return Zone::BELOW;
    if (mgdl > limits_[3]) return Zone::URGENT_ABOVE;
    if (mgdl > limits_[2]) return Zone::ABOVE;
    return Zone::IN_RANGE;
  }

  // 0 or 1 or 2 arrows, and which icon they are, for the trend.
  static uint32_t arrow_icon(Trend t, int &count) {
    switch (t) {
      case Trend::RISING_QUICKLY: count = 2; return 0xF005D;   // arrow-up
      case Trend::RISING: count = 1; return 0xF005D;           // arrow-up
      case Trend::RISING_SLIGHTLY: count = 1; return 0xF005C;  // arrow-top-right
      case Trend::STEADY: count = 1; return 0xF0054;           // arrow-right
      case Trend::FALLING_SLIGHTLY: count = 1; return 0xF0043; // arrow-bottom-right
      case Trend::FALLING: count = 1; return 0xF0045;          // arrow-down
      case Trend::FALLING_QUICKLY: count = 2; return 0xF0045;  // arrow-down
      default: count = 0; return 0;
    }
  }

  std::string age_text(bool &has_age, bool &stale) const {
    has_age = false;
    stale = false;
    if (epoch_ == 0 || time_ == 0) return "";
    const uint32_t age = epoch_ > time_ ? epoch_ - time_ : 0;  // a sensor clock ahead of the screen's reads as now
    has_age = true;
    stale = age > stale_s_;
    const long minutes = static_cast<long>(age / 60);
    if (minutes < 1) return plugin_->text("now");
    if (minutes < 60) return tessera::format(plugin_->text("minutes"), minutes);
    return tessera::format(plugin_->text("hours"), minutes / 60);
  }

  std::string delta_text() const {
    char buf[16];
    const float a = fabsf(delta_);
    if (mmol_) {
      const float r = roundf(a * 10.0f) / 10.0f;
      snprintf(buf, sizeof(buf), "%s%.1f", delta_ == 0 ? "" : (delta_ > 0 ? "+" : "-"), r);
    } else {
      snprintf(buf, sizeof(buf), "%s%ld", delta_ == 0 ? "" : (delta_ > 0 ? "+" : "-"), lroundf(a));
    }
    return buf;
  }

  // Set every part from what is known. ui::set_text, set_font and set_tone change a part only when it differs, so this
  // is free to call every second.
  void paint() {
    bool has_age = false, stale = false;
    const std::string age = age_text(has_age, stale);

    std::string big;
    Tone tone = Tone::NORMAL;
    int arrows = 0;
    uint32_t icon = 0;
    const bool numeric = chosen_ && valid_;
    if (!chosen_) {
      big = plugin_->text("choose");
    } else if (!valid_) {
      big = plugin_->text("no_data");
    } else {
      char buf[16];
      if (mmol_) snprintf(buf, sizeof(buf), "%.1f", reading_);
      else snprintf(buf, sizeof(buf), "%.0f", reading_);
      big = buf;
      const Zone z = zone();
      tone = (z == Zone::URGENT_BELOW || z == Zone::URGENT_ABOVE) ? Tone::ALERT
             : (z == Zone::BELOW || z == Zone::ABOVE)              ? Tone::BUSY
                                                                   : Tone::NORMAL;
      icon = arrow_icon(trend_, arrows);
    }
    // A reading that has gone old is greyed, whatever zone it was in: it says nothing about now.
    if (stale) tone = Tone::NORMAL;
    const theme::Role ink = stale ? theme::MUTED : theme::INK;

    // The arrows' width and the room left for the number.
    const std::string icon_text = arrows > 0 ? ui::icon(icon) : std::string();
    const int gap = ui::px(4);
    const int icon_w = arrows > 0 ? ui::text_width(icon_text, Font::ICON) + ui::px(2) : 0;
    const int arrows_w = arrows > 0 ? arrows * icon_w + (arrows - 1) * ui::px(2) + gap : 0;

    // The largest of the screen's fonts the number fits in, by width and by height. The digits font has nothing but
    // digits, signs and the point, so it only takes a reading.
    Font face = numeric ? Font::DIGITS : Font::HEADLINE;
    const Font faces_num[] = {Font::DIGITS, Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE, Font::BODY};
    const Font faces_word[] = {Font::HEADLINE, Font::TITLE, Font::BODY_LARGE, Font::BODY};
    const Font *list = numeric ? faces_num : faces_word;
    const size_t count = numeric ? sizeof(faces_num) / sizeof(Font) : sizeof(faces_word) / sizeof(Font);
    for (size_t i = 0; i < count; ++i) {
      face = list[i];
      if (face == Font::DIGITS && !ui::covers(big, Font::DIGITS)) continue;
      if (ui::text_width(big, face) + arrows_w <= width_ && ui::line_height(face) <= height_) break;
    }
    ui::set_font(value_, face);
    ui::set_text(value_, big);
    ui::set_tone(value_, tone, ink);

    // Which of the lines under the number fit the height, in the order of what matters.
    const int vh = ui::line_height(face);
    const int row_h = arrows > 0 ? std::max(vh, ui::line_height(Font::ICON)) : vh;
    const int bh = ui::line_height(Font::BODY);
    int used = row_h;
    std::string age_line, delta_line, insulin_line;
    if (numeric && has_age && used + bh <= height_) {
      age_line = age;
      used += bh;
    }
    if (numeric && delta_valid_ && !stale && used + bh <= height_) {
      delta_line = delta_text();
      used += bh;
    }
    if (numeric && insulin_valid_ && used + bh <= height_) {
      char buf[40];
      snprintf(buf, sizeof(buf), " %.2f %s", insulin_value_, insulin_unit_);
      insulin_line = std::string(plugin_->text("insulin")) + buf;
      used += bh;
    }
    ui::set_text(age_, age_line);
    ui::set_tone(age_, stale ? Tone::BUSY : Tone::NORMAL, theme::MUTED);
    ui::set_text(change_label_, delta_line);
    ui::set_tone(change_label_, Tone::NORMAL, ink);
    ui::set_text(insulin_, insulin_line);
    ui::set_tone(insulin_, Tone::NORMAL, theme::MUTED);

    // The number and its arrows centred as one, then the lines under them, all centred in the card.
    const int value_w = std::min(width_, ui::text_width(big, face) + ui::px(2));
    const int total_w = value_w + arrows_w;
    const int x0 = std::max(0, (width_ - total_w) / 2);
    const int top = std::max(0, (height_ - used) / 2);
    lv_obj_set_width(value_, value_w);
    lv_obj_set_pos(value_, x0, top + (row_h - vh) / 2);
    for (int i = 0; i < 2; ++i) {
      if (i < arrows) {
        ui::set_text(arrow_[i], icon_text);
        ui::set_tone(arrow_[i], tone, ink);
        lv_obj_set_width(arrow_[i], icon_w);
        lv_obj_set_pos(arrow_[i], x0 + value_w + gap + i * (icon_w + ui::px(2)),
                       top + (row_h - ui::line_height(Font::ICON)) / 2);
      } else {
        ui::set_text(arrow_[i], "");
      }
    }
    // A line that is empty takes no room, so the ones that are shown stand together under the number.
    int y = top + row_h;
    const std::string *texts[] = {&age_line, &delta_line, &insulin_line};
    lv_obj_t *lines[] = {age_, change_label_, insulin_};
    for (int i = 0; i < 3; ++i) {
      lv_obj_set_pos(lines[i], 0, y);
      if (!texts[i]->empty()) y += bh;
    }
  }

  const tessera::Plugin *plugin_;
  int width_ = 0, height_ = 0;
  uint32_t epoch_ = 0;

  // The options.
  TrendFormat trend_format_ = TrendFormat::TEXT;
  float limits_[4] = {54, 70, 180, 250};
  uint32_t stale_s_ = 15 * 60;

  // What the entities said.
  bool chosen_ = false, valid_ = false, mmol_ = false;
  float reading_ = 0;  // in the glucose sensor's own unit
  Trend trend_ = Trend::UNKNOWN;
  uint32_t time_ = 0;
  bool delta_valid_ = false;
  float delta_ = 0;
  bool insulin_valid_ = false;
  float insulin_value_ = 0;
  char insulin_unit_[12] = "U";

  // The last two different readings the tile saw, for a change when no sensor gives one.
  float cur_value_ = 0, prev_value_ = 0;
  uint32_t cur_time_ = 0, prev_time_ = 0;

  lv_obj_t *value_{}, *arrow_[2]{}, *age_{}, *change_label_{}, *insulin_{};
};

void CgmGlucose::setup() {
  // The id is the tile's id in the manifest; the core makes a GlucoseTile for every card that shows one.
  add_tile("glucose", [this]() { return new GlucoseTile(this); });
}

}  // namespace esphome::cgm_glucose
