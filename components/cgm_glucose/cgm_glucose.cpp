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
  if (!(v > 0) || !std::isfinite(v)) return 0;
  if (v >= EPOCH_SECONDS_BOUND) v /= 1000.0;
  if (v > 4294967295.0) return 0;  // past what 32 bits hold: not a moment
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

// A sensor with no unit of its own is told apart by its value. A CGM reads 40 mg/dL at the lowest, and a reading in
// mmol/L never gets past about 33, so a number up to this is millimoles. Without it a 7.9 would be taken for 7.9 mg/dL
// and shown as an urgent low.
static constexpr float MMOL_GUESS_MAX = 35.0f;

// Whether a reading is in mmol/L: by the unit when the sensor has one (any spelling that says "mmol"), else by the value.
static bool is_mmol(const char *unit, bool have_value, float value) {
  if (unit == nullptr || *unit == '\0') return have_value && value <= MMOL_GUESS_MAX;
  char low[16];
  size_t n = 0;
  for (const char *p = unit; *p != '\0' && n < sizeof(low) - 1; ++p) {
    char ch = *p;
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
    low[n++] = ch;
  }
  low[n] = '\0';
  return strstr(low, "mmol") != nullptr;
}

// The tile, laid out the way SugarTV Card lays out its card: the shape of the slot picks the arrangement.
//   wider than 5:3   a row:     time  reading  arrow  change
//   narrower         a column:  time / reading / arrow change
// with the active insulin above and the forecast under, when the height holds them. The reading is the largest of the
// screen's fonts that fits; when even the smallest does not hold everything, the change goes first and then the time.
// on_state() reads the entities the person chose; on_tick() moves the clock on.
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
    const char *show = c.options["show"] | "clock_forecast";
    clock_ = strncmp(show, "clock", 5) == 0;
    want_forecast_ = strstr(show, "forecast") != nullptr;
    limits_[0] = c.options["urgent_low"] | 54.0f;
    limits_[1] = c.options["low"] | 70.0f;
    limits_[2] = c.options["high"] | 180.0f;
    limits_[3] = c.options["urgent_high"] | 250.0f;
    // The four limits stand in order, whatever the person set: a low under the urgent low would never show.
    limits_[1] = std::max(limits_[1], limits_[0]);
    limits_[2] = std::max(limits_[2], limits_[1]);
    limits_[3] = std::max(limits_[3], limits_[2]);
    stale_s_ = static_cast<uint32_t>(c.options["stale_after"] | 15) * 60u;

    value_ = ui::label(c.parent, Font::VALUE, theme::INK);
    for (lv_obj_t *&a : arrow_) a = ui::label(c.parent, Font::ICON, theme::INK);
    time_ = ui::label(c.parent, Font::BODY, theme::INK);
    delta_ = ui::label(c.parent, Font::BODY, theme::INK);
    insulin_ = ui::label(c.parent, Font::BODY, theme::INK_SOFT);
    lv_obj_set_style_text_align(insulin_, LV_TEXT_ALIGN_CENTER, 0);
    forecast_ = ui::text(c.parent, Font::BODY, theme::INK_SOFT);
    lv_obj_set_style_text_align(forecast_, LV_TEXT_ALIGN_CENTER, 0);
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
      mmol_ = is_mmol(g["attributes"]["unit_of_measurement"] | "", valid_, reading_);
    }

    trend_ = read_trend(entities, g);
    const uint32_t time = read_time(entities, g);
    read_delta(entities, valid_ ? reading_ : 0.0f, time);
    read_insulin(entities);
    time_s_ = time;
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
  Trend read_trend(JsonObjectConst entities, JsonObjectConst g) {
    JsonObjectConst t = entities["trend"];
    trend_source_ = !t.isNull();
    if (trend_source_) {
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
        delta_value_ = dv;
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
      delta_value_ = cur_value_ - prev_value_;
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

  // 0 or 1 or 2 arrows, and which icon they are, for the trend. The icon set has no double chevron: a quick rise or
  // fall draws its arrow twice.
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

  // The time of the reading as the card says it: the clock time of the reading, or how long ago it was. A clock time
  // older than half a day says how long ago instead, since a time of day alone would not say which day.
  std::string time_text(bool &stale) const {
    stale = false;
    if (epoch_ == 0 || time_s_ == 0) return "";
    const uint32_t age = epoch_ > time_s_ ? epoch_ - time_s_ : 0;  // a sensor clock ahead of the screen's reads as now
    stale = age > stale_s_;
    if (clock_ && age < 12u * 3600u) return tessera::clock_text(time_s_);
    const long minutes = static_cast<long>(age / 60);
    if (minutes < 1) return plugin_->text("now");
    if (minutes < 60) return tessera::format(plugin_->text("minutes"), minutes);
    return tessera::format(plugin_->text("hours"), minutes / 60);
  }

  std::string delta_text() const {
    char buf[16];
    const float a = fabsf(delta_value_);
    if (mmol_) {
      const float r = roundf(a * 10.0f) / 10.0f;
      snprintf(buf, sizeof(buf), "%s%.1f", r == 0 ? "" : (delta_value_ > 0 ? "+" : "-"), r);
    } else {
      snprintf(buf, sizeof(buf), "%s%ld", delta_value_ == 0 ? "" : (delta_value_ > 0 ? "+" : "-"), lroundf(a));
    }
    return buf;
  }

  // The arrow written out, as SugarTV Card's forecast line: the range each arrow conventionally stands for. It is the
  // arrow in words, not a calculation.
  std::string forecast_text() const {
    const char *key = nullptr, *mgdl = "", *mmol = "";
    switch (trend_) {
      case Trend::RISING_QUICKLY: key = "rise_over"; mgdl = "45"; mmol = "2.5"; break;
      case Trend::RISING: key = "rise_in"; mgdl = "30-45"; mmol = "1.7-2.5"; break;
      case Trend::RISING_SLIGHTLY: key = "rise_in"; mgdl = "15-30"; mmol = "0.8-1.7"; break;
      case Trend::FALLING_SLIGHTLY: key = "fall_in"; mgdl = "15-30"; mmol = "0.8-1.7"; break;
      case Trend::FALLING: key = "fall_in"; mgdl = "30-45"; mmol = "1.7-2.5"; break;
      case Trend::FALLING_QUICKLY: key = "fall_over"; mgdl = "45"; mmol = "2.5"; break;
      default: return "";
    }
    const std::string text = tessera::fill(plugin_->text(key), "range", mmol_ ? mmol : mgdl);
    return tessera::fill(text.c_str(), "unit", plugin_->text(mmol_ ? "mmoll" : "mgdl"));
  }

  // What paint() has worked out to say, before it knows how big anything can be.
  struct Content {
    std::string big, time, delta, arrow, insulin, forecast;
    bool numeric = false, delta_icon = false, arrow_icon = false;
    int arrows = 0;
  };
  // One arrangement to try: a row or a column, which optional parts it keeps, and the font of the reading.
  struct Plan {
    bool row;
    bool time;
    bool delta;
    Font face;
  };
  // The size of each part in a plan.
  struct Parts {
    Font side = Font::BODY, arrow = Font::ICON, change = Font::BODY;
    int vw = 0, vh = 0, aw = 0, iw = 0, ih = 0, tw = 0, th = 0, dw = 0, dh = 0;
  };

  Parts parts(const Plan &p, const Content &c) const {
    Parts m;
    const bool large = p.face == Font::DIGITS || p.face == Font::VALUE;
    m.side = large ? Font::BODY_LARGE : Font::BODY;
    m.arrow = c.arrow_icon ? ((large || p.face == Font::HEADLINE) ? Font::ICON : Font::ICON_SMALL)
                           : (large ? Font::HEADLINE : Font::BODY);
    m.change = c.delta_icon ? Font::ICON_SMALL : m.side;
    m.vh = ui::line_height(p.face);
    m.vw = ui::text_width(c.big, p.face) + ui::px(2);  // as wide as it is: a plan fits only if the whole text does
    if (c.arrows > 0) {
      m.iw = ui::text_width(c.arrow, m.arrow) + ui::px(2);
      m.ih = ui::line_height(m.arrow);
      m.aw = c.arrows * m.iw + (c.arrows - 1) * ui::px(2);
    }
    if (p.time && !c.time.empty()) {
      m.tw = ui::text_width(c.time, m.side) + ui::px(2);
      m.th = ui::line_height(m.side);
    }
    if (p.delta && !c.delta.empty()) {
      m.dw = ui::text_width(c.delta, m.change) + ui::px(2);
      m.dh = ui::line_height(m.change);
    }
    return m;
  }

  // How wide and how high the parts of a plan stand together.
  static void measure(const Plan &p, const Parts &m, int &w, int &h) {
    const int g = ui::px(p.row ? 6 : 4);
    w = m.vw;
    h = m.vh;
    if (p.row) {
      const int widths[] = {m.tw, m.aw, m.dw};
      const int heights[] = {m.th, m.ih, m.dh};
      for (int i = 0; i < 3; ++i) {
        if (widths[i] <= 0) continue;
        w += g + widths[i];
        h = std::max(h, heights[i]);
      }
      return;
    }
    if (m.tw > 0) {
      w = std::max(w, m.tw);
      h += m.th + g;
    }
    const int rw = m.aw + (m.aw > 0 && m.dw > 0 ? g : 0) + m.dw;
    if (rw > 0) {
      w = std::max(w, rw);
      h += std::max(m.ih, m.dh) + g;
    }
  }

  static void put(lv_obj_t *part, int x, int y, int w) {
    lv_obj_set_pos(part, x, y);
    lv_obj_set_width(part, w);
  }

  // Put the parts of a plan in the box (x0, y0, w, h) that measure() gave. Every position is a number kept here, never
  // read back from LVGL.
  void place(const Plan &p, const Parts &m, const Content &c, int x0, int y0, int w, int h) {
    const int g = ui::px(p.row ? 6 : 4);
    const int ag = ui::px(2);
    ui::set_font(time_, m.side);
    ui::set_text(time_, m.tw > 0 ? c.time : std::string());
    ui::set_font(delta_, m.change);
    ui::set_text(delta_, m.dw > 0 ? c.delta : std::string());
    for (int i = 0; i < 2; ++i) {
      ui::set_font(arrow_[i], m.arrow);
      ui::set_text(arrow_[i], i < c.arrows ? c.arrow : std::string());
    }
    // The arrows, side by side, from x and with their top at y in a row `rowh` high.
    auto arrows_at = [&](int x, int y, int rowh) {
      for (int i = 0; i < c.arrows; ++i) put(arrow_[i], x + i * (m.iw + ag), y + (rowh - m.ih) / 2, m.iw);
    };
    if (p.row) {
      int x = x0;
      if (m.tw > 0) {
        put(time_, x, y0 + (h - m.th) / 2, m.tw);
        x += m.tw + g;
      }
      put(value_, x, y0 + (h - m.vh) / 2, m.vw);
      x += m.vw;
      if (m.aw > 0) {
        x += g;
        arrows_at(x, y0, h);
        x += m.aw;
      }
      if (m.dw > 0) put(delta_, x + g, y0 + (h - m.dh) / 2, m.dw);
      return;
    }
    int y = y0;
    if (m.tw > 0) {
      put(time_, x0 + (w - m.tw) / 2, y, m.tw);
      y += m.th + g;
    }
    put(value_, x0 + (w - m.vw) / 2, y, m.vw);
    y += m.vh + g;
    const int rw = m.aw + (m.aw > 0 && m.dw > 0 ? g : 0) + m.dw;
    if (rw > 0) {
      const int rh = std::max(m.ih, m.dh);
      int x = x0 + (w - rw) / 2;
      if (m.aw > 0) {
        arrows_at(x, y, rh);
        x += m.aw + g;
      }
      if (m.dw > 0) put(delta_, x, y + (rh - m.dh) / 2, m.dw);
    }
  }

  // Set every part from what is known. ui::set_text, set_font and set_tone change a part only when it differs, so this
  // is free to call every second.
  void paint() {
    bool stale = false;
    Content c;
    c.time = time_text(stale);

    Tone tone = Tone::NORMAL;
    c.numeric = chosen_ && valid_;
    if (!chosen_) {
      c.big = plugin_->text("choose");
    } else if (!valid_) {
      c.big = plugin_->text("no_data");
    } else {
      char buf[16];
      if (mmol_) snprintf(buf, sizeof(buf), "%.1f", reading_);
      else snprintf(buf, sizeof(buf), "%.0f", reading_);
      c.big = buf;
      const Zone z = zone();
      tone = (z == Zone::URGENT_BELOW || z == Zone::URGENT_ABOVE) ? Tone::ALERT
             : (z == Zone::BELOW || z == Zone::ABOVE)              ? Tone::BUSY
                                                                   : Tone::NORMAL;
      int n = 0;
      const uint32_t icon = arrow_icon(trend_, n);
      if (n > 0) {
        c.arrows = n;
        c.arrow = ui::icon(icon);
        c.arrow_icon = true;
      } else if (trend_source_) {
        c.arrows = 1;  // a trend sensor that says nothing the tile knows: a question mark, as SugarTV Card draws
        c.arrow = "?";
      }
      // The change, or the clock that says it is not known yet. An old reading has no change to show.
      if (!stale) {
        if (delta_valid_) {
          c.delta = delta_text();
        } else {
          c.delta = ui::icon(0xF0996);  // progress-clock
          c.delta_icon = true;
        }
      }
      if (insulin_valid_) {
        char buf2[40];
        snprintf(buf2, sizeof(buf2), " %.2f %s", insulin_value_, insulin_unit_);
        c.insulin = std::string(plugin_->text("insulin")) + buf2;
      }
      if (want_forecast_) c.forecast = forecast_text();
    }
    if (!c.numeric) c.time.clear();
    // A reading that has gone old is greyed, whatever zone it was in: it says nothing about now.
    if (stale) tone = Tone::NORMAL;
    const theme::Role ink = stale ? theme::MUTED : theme::INK;
    const theme::Role soft = stale ? theme::MUTED : theme::INK_SOFT;

    // The arrangement: the slot's shape picks row or column, and the largest font of the screen the parts fit in. The
    // digits font has nothing but digits, signs and the point, so it only takes a reading. First the big fonts, keeping
    // every part; then, only if none fits, a smaller font, and the parts go: the change, then the time.
    const bool wide = width_ * 3 > height_ * 5;
    const Font faces_num[] = {Font::DIGITS, Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE, Font::BODY};
    const Font faces_word[] = {Font::HEADLINE, Font::TITLE, Font::BODY_LARGE, Font::BODY};
    const Font *faces = c.numeric ? faces_num : faces_word;
    const size_t count = c.numeric ? sizeof(faces_num) / sizeof(Font) : sizeof(faces_word) / sizeof(Font);
    const Font any = faces[0];
    const Plan stages[] = {{wide, true, true, any},   {!wide, true, true, any},  {wide, true, false, any},
                           {!wide, true, false, any}, {!wide, false, false, any}};
    Plan plan{false, false, false, faces[count - 1]};
    Parts m = parts(plan, c);
    int w = 0, h = 0;
    bool found = false;
    for (int pass = 0; pass < 2 && !found; ++pass) {
      const size_t limit = pass == 0 ? count - 2 : count;
      for (const Plan &stage : stages) {
        for (size_t i = 0; i < limit && !found; ++i) {
          Plan p = stage;
          p.face = faces[i];
          if (p.face == Font::DIGITS && !ui::covers(c.big, Font::DIGITS)) continue;
          const Parts pm = parts(p, c);
          int pw = 0, ph = 0;
          measure(p, pm, pw, ph);
          if (pw <= width_ && ph <= height_) {
            plan = p;
            m = pm;
            w = pw;
            h = ph;
            found = true;
          }
        }
        if (found) break;
      }
    }
    if (!found) {
      m.vw = std::min(m.vw, width_);  // nothing holds it whole: the label cuts it with dots
      measure(plan, m, w, h);
    }

    // What the height has left: the insulin above the reading, then the forecast under it.
    const int bh = ui::line_height(Font::BODY);
    const int gap = ui::px(4);
    int room = height_ - h;
    const bool with_insulin = !c.insulin.empty() && room >= bh + gap;
    if (with_insulin) room -= bh + gap;
    int forecast_lines = 0;
    if (!c.forecast.empty()) {
      // A forecast wraps; a word that breaks wastes some of every line, so it counts a tenth more.
      const int lines = std::max(1, (ui::text_width(c.forecast, Font::BODY) * 11 / 10 + width_ - 1) / width_);
      if (lines <= 3 && room >= lines * bh + gap) forecast_lines = lines;
    }
    const int total = h + (with_insulin ? bh + gap : 0) + (forecast_lines > 0 ? forecast_lines * bh + gap : 0);
    int y = std::max(0, (height_ - total) / 2);

    ui::set_text(insulin_, with_insulin ? c.insulin : std::string());
    ui::set_tone(insulin_, Tone::NORMAL, soft);
    if (with_insulin) {
      put(insulin_, 0, y, width_);
      y += bh + gap;
    }

    ui::set_font(value_, plan.face);
    ui::set_text(value_, c.big);
    ui::set_tone(value_, tone, ink);
    place(plan, m, c, std::max(0, (width_ - w) / 2), y, w, h);
    for (lv_obj_t *a : arrow_) ui::set_tone(a, tone, ink);
    ui::set_tone(time_, Tone::NORMAL, ink);
    ui::set_tone(delta_, Tone::NORMAL, c.delta_icon ? theme::MUTED : ink);
    y += h;

    ui::set_text(forecast_, forecast_lines > 0 ? c.forecast : std::string());
    ui::set_tone(forecast_, Tone::NORMAL, soft);
    if (forecast_lines > 0) {
      lv_obj_set_pos(forecast_, 0, y + gap);
      lv_obj_set_size(forecast_, width_, forecast_lines * bh);
    }
  }

  const tessera::Plugin *plugin_;
  int width_ = 0, height_ = 0;
  uint32_t epoch_ = 0;

  // The options.
  TrendFormat trend_format_ = TrendFormat::TEXT;
  bool clock_ = true, want_forecast_ = true;
  float limits_[4] = {54, 70, 180, 250};
  uint32_t stale_s_ = 15 * 60;

  // What the entities said.
  bool chosen_ = false, valid_ = false, mmol_ = false, trend_source_ = false;
  float reading_ = 0;  // in the glucose sensor's own unit
  Trend trend_ = Trend::UNKNOWN;
  uint32_t time_s_ = 0;  // when the reading was taken
  bool delta_valid_ = false;
  float delta_value_ = 0;
  bool insulin_valid_ = false;
  float insulin_value_ = 0;
  char insulin_unit_[12] = "U";

  // The last two different readings the tile saw, for a change when no sensor gives one.
  float cur_value_ = 0, prev_value_ = 0;
  uint32_t cur_time_ = 0, prev_time_ = 0;

  lv_obj_t *value_{}, *arrow_[2]{}, *time_{}, *delta_{}, *insulin_{}, *forecast_{};
};

void CgmGlucose::setup() {
  // The id is the tile's id in the manifest; the core makes a GlucoseTile for every card that shows one.
  add_tile("glucose", [this]() { return new GlucoseTile(this); });
}

}  // namespace esphome::cgm_glucose
