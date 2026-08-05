#include "performance.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <ctime>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <strata/strata.h>

#include "data/json.hpp"

namespace strata::desktop {
namespace {

using data::JsonValue;
using Clock = std::chrono::steady_clock;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values = {}) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] const JsonValue& required(const JsonValue& source, const std::string_view field) {
    const JsonValue* value = source.find(field);
    if (value == nullptr) {
        throw std::invalid_argument("performance scenario is missing '" + std::string(field) + "'");
    }
    return *value;
}

[[nodiscard]] const JsonValue* optional(
    const JsonValue& source,
    const std::string_view field
) noexcept {
    return source.find(field);
}

void require_object(const JsonValue& value, const std::string_view label) {
    if (value.object() == nullptr) {
        throw std::invalid_argument(std::string(label) + " must be a JSON object");
    }
}

[[nodiscard]] const JsonValue::Array& require_array(
    const JsonValue& value,
    const std::string_view label
) {
    if (value.array() == nullptr) {
        throw std::invalid_argument(std::string(label) + " must be a JSON array");
    }
    return *value.array();
}

[[nodiscard]] std::string text(const JsonValue& value, const std::string_view label) {
    if (value.string() == nullptr) {
        throw std::invalid_argument(std::string(label) + " must be a string");
    }
    return *value.string();
}

[[nodiscard]] double number(const JsonValue& value, const std::string_view label) {
    if (value.number() != nullptr) return *value.number();
    if (value.integer() != nullptr) return static_cast<double>(*value.integer());
    throw std::invalid_argument(std::string(label) + " must be a number");
}

[[nodiscard]] std::int64_t integer(const JsonValue& value, const std::string_view label) {
    if (value.integer() == nullptr) {
        throw std::invalid_argument(std::string(label) + " must be an integer");
    }
    return *value.integer();
}

[[nodiscard]] bool boolean(const JsonValue& value, const std::string_view label) {
    if (value.boolean() == nullptr) {
        throw std::invalid_argument(std::string(label) + " must be a boolean");
    }
    return *value.boolean();
}

[[nodiscard]] std::uint32_t positive_count(
    const JsonValue& value,
    const std::string_view label,
    const bool allow_zero = false
) {
    const std::int64_t parsed = integer(value, label);
    const std::int64_t minimum = allow_zero ? 0 : 1;
    if (parsed < minimum || parsed > 1'000'000) {
        throw std::invalid_argument(std::string(label) + " is outside the supported range");
    }
    return static_cast<std::uint32_t>(parsed);
}

[[nodiscard]] host::Selector selector(const JsonValue& value, const std::string_view label) {
    if (value.string() != nullptr) return host::Selector{.key = *value.string()};
    require_object(value, label);
    host::Selector result;
    if (const JsonValue* field = optional(value, "path"); field != nullptr) {
        result.path = text(*field, std::string(label) + ".path");
    }
    if (const JsonValue* field = optional(value, "key"); field != nullptr) {
        result.key = text(*field, std::string(label) + ".key");
    }
    if (const JsonValue* field = optional(value, "name"); field != nullptr) {
        result.name = text(*field, std::string(label) + ".name");
    }
    if (const JsonValue* field = optional(value, "role"); field != nullptr) {
        result.role = text(*field, std::string(label) + ".role");
    }
    if (const JsonValue* field = optional(value, "x"); field != nullptr) {
        result.x = number(*field, std::string(label) + ".x");
    }
    if (const JsonValue* field = optional(value, "y"); field != nullptr) {
        result.y = number(*field, std::string(label) + ".y");
    }
    const bool coordinates = result.x.has_value() || result.y.has_value();
    if (coordinates != (result.x.has_value() && result.y.has_value())) {
        throw std::invalid_argument(std::string(label) + " coordinates require x and y");
    }
    if (!coordinates && !result.path.has_value() && !result.key.has_value() &&
        !result.name.has_value() && !result.role.has_value()) {
        throw std::invalid_argument(std::string(label) + " requires a selector");
    }
    return result;
}

[[nodiscard]] PerformanceStep performance_step(
    const JsonValue& value,
    const std::string_view label
) {
    require_object(value, label);
    if (value.object()->size() != 1U) {
        throw std::invalid_argument(std::string(label) + " must contain one operation");
    }
    const auto& [operation, argument] = value.object()->front();
    if (operation == "frames") {
        return PerformanceFramesStep{positive_count(argument, std::string(label) + ".frames")};
    }
    if (operation == "click") return PerformanceClickStep{selector(argument, "click")};
    if (operation == "move") return PerformanceMoveStep{selector(argument, "move")};
    if (operation == "drag") {
        require_object(argument, "drag");
        PerformanceDragStep result;
        result.target = selector(argument, "drag");
        if (const JsonValue* field = optional(argument, "fromFraction");
            field != nullptr) {
            result.from_fraction = number(*field, "drag.fromFraction");
        }
        if (const JsonValue* field = optional(argument, "toFraction");
            field != nullptr) {
            result.to_fraction = number(*field, "drag.toFraction");
        }
        if (const JsonValue* field = optional(argument, "moves"); field != nullptr) {
            result.moves = positive_count(*field, "drag.moves");
        }
        if (!std::isfinite(result.from_fraction) ||
            !std::isfinite(result.to_fraction) ||
            result.from_fraction < 0.0 || result.from_fraction > 1.0 ||
            result.to_fraction < 0.0 || result.to_fraction > 1.0 ||
            result.from_fraction == result.to_fraction) {
            throw std::invalid_argument(
                "drag fractions must be distinct finite values from zero through one"
            );
        }
        return result;
    }
    if (operation == "scroll") {
        require_object(argument, "scroll");
        PerformanceScrollStep result;
        result.target = selector(argument, "scroll");
        if (const JsonValue* field = optional(argument, "deltaX"); field != nullptr) {
            result.delta_x = number(*field, "scroll.deltaX");
        }
        if (const JsonValue* field = optional(argument, "deltaY"); field != nullptr) {
            result.delta_y = number(*field, "scroll.deltaY");
        }
        if (!std::isfinite(result.delta_x) || !std::isfinite(result.delta_y) ||
            (result.delta_x == 0.0 && result.delta_y == 0.0)) {
            throw std::invalid_argument("scroll requires a finite nonzero delta");
        }
        return result;
    }
    if (operation == "key") return PerformanceKeyStep{text(argument, "key")};
    throw std::invalid_argument(std::string(label) + " has unknown operation '" + operation + "'");
}

[[nodiscard]] std::vector<PerformanceStep> performance_steps(
    const JsonValue* value,
    const std::string_view label
) {
    if (value == nullptr) return {};
    const JsonValue::Array& values = require_array(*value, label);
    std::vector<PerformanceStep> result;
    result.reserve(values.size());
    for (std::size_t index = 0U; index < values.size(); ++index) {
        result.push_back(performance_step(values[index], std::string(label) + "[" +
            std::to_string(index) + "]"));
    }
    return result;
}

[[nodiscard]] std::string selector_label(const host::Selector& selector) {
    if (selector.key.has_value()) return "key=" + *selector.key;
    if (selector.role.has_value() || selector.name.has_value()) {
        return selector.role.value_or("*") + ":" + selector.name.value_or("*");
    }
    if (selector.path.has_value()) return "path=" + *selector.path;
    return "coordinates";
}

[[nodiscard]] std::uint32_t virtual_key(const std::string_view key) {
    if (key == "f2") return VK_F2;
    if (key == "f3") return VK_F3;
    if (key == "f4") return VK_F4;
    if (key == "f6") return VK_F6;
    if (key == "f7") return VK_F7;
    if (key == "f8") return VK_F8;
    if (key == "f9") return VK_F9;
    if (key == "f10") return VK_F10;
    if (key == "tab") return VK_TAB;
    if (key == "enter") return VK_RETURN;
    if (key == "space") return VK_SPACE;
    if (key == "escape") return VK_ESCAPE;
    if (key == "left") return VK_LEFT;
    if (key == "right") return VK_RIGHT;
    if (key == "up") return VK_UP;
    if (key == "down") return VK_DOWN;
    if (key == "home") return VK_HOME;
    if (key == "end") return VK_END;
    if (key.size() == 1U && key.front() >= 'a' && key.front() <= 'z') {
        return static_cast<std::uint32_t>('A' + key.front() - 'a');
    }
    throw std::invalid_argument("performance key '" + std::string(key) + "' is unsupported");
}

[[nodiscard]] std::int64_t nanos_since(const Clock::time_point started) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
}

[[nodiscard]] double millis(const std::int64_t nanos) noexcept {
    return static_cast<double>(nanos) / 1'000'000.0;
}

[[nodiscard]] std::string timestamp_utc() {
    const std::time_t current = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()
    );
    std::tm broken{};
    gmtime_s(&broken, &current);
    std::ostringstream output;
    output << std::put_time(&broken, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

[[nodiscard]] std::string environment_text(const wchar_t* name) {
    const DWORD required_size = GetEnvironmentVariableW(name, nullptr, 0U);
    if (required_size == 0U) return {};
    std::wstring wide(required_size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, wide.data(), required_size);
    if (written == 0U || written >= required_size) return {};
    wide.resize(written);
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr
    );
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        result.data(), size, nullptr, nullptr
    ));
    return result;
}

[[nodiscard]] std::string priority_name(const DWORD value) {
    switch (value) {
    case IDLE_PRIORITY_CLASS: return "idle";
    case BELOW_NORMAL_PRIORITY_CLASS: return "below-normal";
    case NORMAL_PRIORITY_CLASS: return "normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "above-normal";
    case HIGH_PRIORITY_CLASS: return "high";
    case REALTIME_PRIORITY_CLASS: return "realtime";
    default: return "unknown";
    }
}

struct Distribution final {
    std::size_t count = 0U;
    std::int64_t total = 0;
    std::int64_t minimum = 0;
    std::int64_t average = 0;
    std::int64_t p50 = 0;
    std::int64_t p95 = 0;
    std::int64_t p99 = 0;
    std::int64_t maximum = 0;
    double standard_deviation = 0.0;
    double average_fps = 0.0;
    double one_percent_low_fps = 0.0;
};

[[nodiscard]] Distribution distribution(std::vector<std::int64_t> values) {
    Distribution result;
    result.count = values.size();
    if (values.empty()) return result;
    std::ranges::sort(values);
    const auto percentile = [&values](const double fraction) {
        const std::size_t rank = static_cast<std::size_t>(std::ceil(
            fraction * static_cast<double>(values.size())
        ));
        return values[std::clamp(rank, std::size_t{1U}, values.size()) - 1U];
    };
    long double total = 0.0L;
    for (const std::int64_t value : values) total += static_cast<long double>(value);
    result.total = total > static_cast<long double>(std::numeric_limits<std::int64_t>::max())
        ? std::numeric_limits<std::int64_t>::max()
        : static_cast<std::int64_t>(total);
    result.minimum = values.front();
    result.average = static_cast<std::int64_t>(std::llround(total / values.size()));
    result.p50 = percentile(0.50);
    result.p95 = percentile(0.95);
    result.p99 = percentile(0.99);
    result.maximum = values.back();
    long double squared = 0.0L;
    for (const std::int64_t value : values) {
        const long double delta = static_cast<long double>(value) - result.average;
        squared += delta * delta;
    }
    result.standard_deviation = std::sqrt(static_cast<double>(squared / values.size()));
    if (result.average > 0) result.average_fps = 1'000'000'000.0 / result.average;
    const std::size_t slow_count = std::max<std::size_t>(
        1U, static_cast<std::size_t>(std::ceil(values.size() * 0.01))
    );
    long double slow_total = 0.0L;
    for (std::size_t index = values.size() - slow_count; index < values.size(); ++index) {
        slow_total += values[index];
    }
    const long double slow_average = slow_total / slow_count;
    if (slow_average > 0.0L) {
        result.one_percent_low_fps = 1'000'000'000.0 / static_cast<double>(slow_average);
    }
    return result;
}

[[nodiscard]] JsonValue distribution_json(const Distribution& value) {
    return object({
        {"averageFps", JsonValue(value.average_fps)},
        {"averageMillis", JsonValue(millis(value.average))},
        {"count", JsonValue(static_cast<std::int64_t>(value.count))},
        {"maxMillis", JsonValue(millis(value.maximum))},
        {"minMillis", JsonValue(millis(value.minimum))},
        {"onePercentLowFps", JsonValue(value.one_percent_low_fps)},
        {"p50Millis", JsonValue(millis(value.p50))},
        {"p95Millis", JsonValue(millis(value.p95))},
        {"p99Millis", JsonValue(millis(value.p99))},
        {"standardDeviationMillis", JsonValue(value.standard_deviation / 1'000'000.0)},
        {"totalMillis", JsonValue(millis(value.total))},
    });
}

void write_file(const std::filesystem::path& path, const std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) throw std::runtime_error("could not write performance artifact: " + path.string());
}

[[nodiscard]] std::string html_report(std::string report_json) {
    std::size_t offset = 0U;
    while ((offset = report_json.find("</", offset)) != std::string::npos) {
        report_json.insert(offset + 1U, "\\");
        offset += 3U;
    }
    std::ostringstream html;
    html << R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>Strata desktop performance</title>
<style>
:root{color-scheme:dark;font:14px system-ui;background:#0b0e14;color:#e6edf3}body{margin:0;padding:28px}.head{display:flex;justify-content:space-between;gap:20px;align-items:end}.muted{color:#8b949e}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin:22px 0}.card,section{background:#151a23;border:1px solid #303846;border-radius:10px;padding:16px}.big{font-size:26px;font-weight:700;margin-top:5px}.ok{color:#62d68b}.bad{color:#ff7b72}table{width:100%;border-collapse:collapse}th,td{text-align:right;padding:8px;border-bottom:1px solid #303846}th:first-child,td:first-child{text-align:left}canvas{width:100%;height:220px;background:#0d1118;border-radius:6px}section{margin:14px 0}code{color:#9ecbff}
</style></head><body><div class="head"><div><h1>Strata desktop performance</h1><div id="subtitle" class="muted"></div></div><div id="validity"></div></div><div id="cards" class="cards"></div><section><h2>Measured phases</h2><table><thead><tr><th>Phase</th><th>Frames</th><th>Avg FPS</th><th>1% low</th><th>p50 ms</th><th>p95 ms</th><th>p99 ms</th><th>Max ms</th><th>Spikes</th></tr></thead><tbody id="phases"></tbody></table></section><section><h2>Frame-time timeline</h2><canvas id="timeline" width="1500" height="260"></canvas><div class="muted">Each phase uses its own adaptive spike threshold. Red bars exceed it.</div></section><section><h2>Run validity</h2><ul id="reasons"></ul></section><section><h2>Environment</h2><pre id="environment"></pre></section>
<script>const report=)HTML" << report_json << R"HTML(;
const fmt=n=>Number(n).toFixed(2);document.querySelector('#subtitle').textContent=`${report.scenario} · ${report.generatedAtUtc}`;const comparison=report.comparison;let validity=report.valid?'<b class="ok">VALID MEASUREMENT RUN</b>':'<b class="bad">INVALID RUN</b>';if(comparison&&comparison.status&&comparison.status!=='pass')validity+=` · <b class="bad">BASELINE ${comparison.status.toUpperCase()}</b>`;else if(comparison&&comparison.status==='pass')validity+=' · <b class="ok">BASELINE PASS</b>';document.querySelector('#validity').innerHTML=validity;const total=report.summary.total;document.querySelector('#cards').innerHTML=[['Average FPS',fmt(total.averageFps)],['1% low FPS',fmt(total.onePercentLowFps)],['p95 frame',fmt(total.p95Millis)+' ms'],['Worst frame',fmt(total.maxMillis)+' ms'],['Cold ready',fmt(report.startup.readyMillis)+' ms']].map(x=>`<div class="card"><div class="muted">${x[0]}</div><div class="big">${x[1]}</div></div>`).join('');document.querySelector('#phases').innerHTML=report.phases.map(p=>`<tr><td>${p.name}</td><td>${p.total.count}</td><td>${fmt(p.total.averageFps)}</td><td>${fmt(p.total.onePercentLowFps)}</td><td>${fmt(p.total.p50Millis)}</td><td>${fmt(p.total.p95Millis)}</td><td>${fmt(p.total.p99Millis)}</td><td>${fmt(p.total.maxMillis)}</td><td>${p.spikes.length}</td></tr>`).join('');const reasons=[...report.invalidReasons,...(comparison&&comparison.reasons?comparison.reasons:[])];document.querySelector('#reasons').innerHTML=(reasons.length?reasons:['No invalidating conditions observed.']).map(x=>`<li>${x}</li>`).join('');document.querySelector('#environment').textContent=JSON.stringify(report.environment,null,2);const frames=report.phases.flatMap((p,pi)=>p.frames.map(f=>({...f,pi,threshold:p.spikeThresholdMillis})));const c=document.querySelector('#timeline'),g=c.getContext('2d'),max=Math.max(1,...frames.map(f=>f.totalMillis));g.clearRect(0,0,c.width,c.height);frames.forEach((f,i)=>{const x=i*c.width/Math.max(frames.length,1),w=Math.max(1,c.width/Math.max(frames.length,1)),h=f.totalMillis/max*(c.height-24);g.fillStyle=!f.valid?'#d29922':f.totalMillis>f.threshold?'#ff7b72':['#58a6ff','#a371f7','#3fb950','#f0883e'][f.pi%4];g.fillRect(x,c.height-h,w,h)});g.fillStyle='#8b949e';g.fillText(`max ${fmt(max)} ms`,8,14);
</script></body></html>)HTML";
    return html.str();
}

} // namespace

PerformanceScenario load_performance_scenario(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open performance scenario: " + path.string());
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const JsonValue document = data::parse_json(source);
    require_object(document, "performance scenario");
    PerformanceScenario result;
    std::uint64_t fingerprint = 14695981039346656037ULL;
    for (const char character : source) {
        fingerprint ^= static_cast<unsigned char>(character);
        fingerprint *= 1099511628211ULL;
    }
    std::ostringstream fingerprint_text;
    fingerprint_text << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0')
                     << fingerprint;
    result.workload_fingerprint = fingerprint_text.str();
    const std::int64_t version = integer(required(document, "version"), "version");
    if (version != 1) throw std::invalid_argument("performance scenario version must be 1");
    result.name = text(required(document, "name"), "name");
    if (result.name.empty()) throw std::invalid_argument("performance scenario name is empty");
    if (const JsonValue* window = optional(document, "window"); window != nullptr) {
        require_object(*window, "window");
        result.client_width = positive_count(required(*window, "width"), "window.width");
        result.client_height = positive_count(required(*window, "height"), "window.height");
    }
    if (const JsonValue* validity = optional(document, "validity"); validity != nullptr) {
        require_object(*validity, "validity");
        if (const JsonValue* value = optional(*validity, "requireVisible"); value != nullptr) {
            result.require_visible = boolean(*value, "validity.requireVisible");
        }
        if (const JsonValue* value = optional(*validity, "requireForeground"); value != nullptr) {
            result.require_foreground = boolean(*value, "validity.requireForeground");
        }
        if (const JsonValue* value = optional(*validity, "requireDraws"); value != nullptr) {
            result.require_draws = boolean(*value, "validity.requireDraws");
        }
        if (result.require_foreground && !result.require_visible) {
            throw std::invalid_argument("foreground performance scenarios must require visibility");
        }
    }
    if (const JsonValue* spikes = optional(document, "spikes"); spikes != nullptr) {
        require_object(*spikes, "spikes");
        if (const JsonValue* value = optional(*spikes, "floorMillis"); value != nullptr) {
            result.spike_floor_millis = number(*value, "spikes.floorMillis");
        }
        if (const JsonValue* value = optional(*spikes, "baselineMultiplier"); value != nullptr) {
            result.spike_multiplier = number(*value, "spikes.baselineMultiplier");
        }
        if (const JsonValue* value = optional(*spikes, "maximumReported"); value != nullptr) {
            result.maximum_spikes = positive_count(*value, "spikes.maximumReported");
        }
    }
    if (!std::isfinite(result.spike_floor_millis) || result.spike_floor_millis < 0.0 ||
        !std::isfinite(result.spike_multiplier) || result.spike_multiplier < 1.0) {
        throw std::invalid_argument("performance spike policy is invalid");
    }
    if (const JsonValue* regression = optional(document, "regression"); regression != nullptr) {
        require_object(*regression, "regression");
        if (const JsonValue* value = optional(*regression, "maximumAveragePercent");
            value != nullptr) {
            result.maximum_average_regression_percent = number(
                *value, "regression.maximumAveragePercent"
            );
        }
        if (const JsonValue* value = optional(*regression, "maximumP95Percent");
            value != nullptr) {
            result.maximum_p95_regression_percent = number(
                *value, "regression.maximumP95Percent"
            );
        }
        if (const JsonValue* value = optional(*regression, "maximumP99Percent");
            value != nullptr) {
            result.maximum_p99_regression_percent = number(
                *value, "regression.maximumP99Percent"
            );
        }
        if (const JsonValue* value = optional(*regression, "minimumAbsoluteMillis");
            value != nullptr) {
            result.minimum_regression_millis = number(
                *value, "regression.minimumAbsoluteMillis"
            );
        }
    }
    for (const double threshold : {
             result.maximum_average_regression_percent,
             result.maximum_p95_regression_percent,
             result.maximum_p99_regression_percent,
         }) {
        if (!std::isfinite(threshold) || threshold < 0.0) {
            throw std::invalid_argument("performance regression threshold is invalid");
        }
    }
    if (!std::isfinite(result.minimum_regression_millis) ||
        result.minimum_regression_millis < 0.0) {
        throw std::invalid_argument("performance absolute regression threshold is invalid");
    }
    result.setup = performance_steps(optional(document, "setup"), "setup");
    const JsonValue::Array& phases = require_array(required(document, "phases"), "phases");
    std::set<std::string, std::less<>> phase_names;
    for (std::size_t index = 0U; index < phases.size(); ++index) {
        const JsonValue& value = phases[index];
        require_object(value, "phase");
        PerformancePhase phase;
        phase.name = text(required(value, "name"), "phase.name");
        if (phase.name.empty()) throw std::invalid_argument("performance phase name is empty");
        if (!phase_names.insert(phase.name).second) {
            throw std::invalid_argument("duplicate performance phase name: " + phase.name);
        }
        if (const JsonValue* field = optional(value, "warmupFrames"); field != nullptr) {
            phase.warmup_frames = positive_count(*field, "phase.warmupFrames", true);
        }
        if (const JsonValue* field = optional(value, "warmupIterations"); field != nullptr) {
            phase.warmup_iterations = positive_count(*field, "phase.warmupIterations", true);
        }
        if (const JsonValue* field = optional(value, "iterations"); field != nullptr) {
            phase.iterations = positive_count(*field, "phase.iterations");
        }
        phase.steps = performance_steps(optional(value, "steps"), "phase.steps");
        if (phase.steps.empty()) throw std::invalid_argument("performance phase has no steps");
        result.phases.push_back(std::move(phase));
    }
    if (result.phases.empty()) throw std::invalid_argument("performance scenario has no phases");
    return result;
}

struct PerformanceRunner::Impl final {
    struct FrameRecord final {
        DesktopFrameSample sample;
        std::uint64_t sequence = 0U;
        std::uint32_t iteration = 0U;
        std::string operation;
        bool valid = true;
    };

    struct PhaseResult final {
        PerformancePhase phase;
        std::vector<FrameRecord> frames;
    };

    Impl(
        HWND window,
        Host& host,
        PerformanceScenario scenario,
        std::filesystem::path output_root,
        std::filesystem::path baseline_report,
        PerformanceStartup startup
    ) : window(window), host(host), scenario(std::move(scenario)),
        output_root(std::move(output_root)), baseline_report(std::move(baseline_report)),
        startup(startup), host_info(host.performance_info()),
        expected_monitor(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)),
        expected_dpi(GetDpiForWindow(window)) {}

    void invalidate(std::string reason) {
        if (!std::ranges::contains(invalid_reasons, reason)) {
            invalid_reasons.push_back(std::move(reason));
        }
    }

    [[nodiscard]] bool frame_valid(const DesktopFrameSample& sample) {
        bool valid = true;
        if (!IsWindow(window)) {
            invalidate("The benchmark window was destroyed during measurement.");
            return false;
        }
        if (scenario.require_visible && (!IsWindowVisible(window) || IsIconic(window))) {
            invalidate("The benchmark window was hidden or minimized during measurement.");
            valid = false;
        }
        RECT client{};
        if (!GetClientRect(window, &client) ||
            client.right - client.left != static_cast<LONG>(scenario.client_width) ||
            client.bottom - client.top != static_cast<LONG>(scenario.client_height)) {
            invalidate("The benchmark client dimensions changed during measurement.");
            valid = false;
        }
        if (GetDpiForWindow(window) != expected_dpi ||
            MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) != expected_monitor) {
            invalidate("The benchmark window changed DPI or monitor during measurement.");
            valid = false;
        }
        if (scenario.require_foreground && GetForegroundWindow() != window) {
            invalidate("The benchmark window lost foreground focus during measurement.");
            valid = false;
        }
        if (scenario.require_visible && !sample.presented) {
            invalidate("DXGI reported an occluded presentation during measurement.");
            valid = false;
        }
        if (scenario.require_draws && !sample.had_draws) {
            invalidate("The measured frame contained no render draws.");
            valid = false;
        }
        return valid;
    }

    void run_frame(
        const bool measured,
        PhaseResult* phase,
        const std::uint32_t iteration,
        std::string operation
    ) {
        if (!pump_messages()) {
            invalidate("The Windows message loop was interrupted during measurement.");
            throw std::runtime_error("performance benchmark message loop was interrupted");
        }
        const Clock::time_point frame_started = Clock::now();
        host.frame();
        DesktopFrameSample sample = host.last_frame_sample();
        sample.total_nanos = nanos_since(frame_started);
        if (first_frame.total_nanos == 0 && sample.had_draws) first_frame = sample;
        if (!measured || phase == nullptr) return;
        const bool valid = frame_valid(sample);
        phase->frames.push_back(FrameRecord{
            sample, ++sequence, iteration, std::move(operation), valid,
        });
    }

    [[nodiscard]] std::pair<double, double> resolve(const host::Selector& selector) {
        const DesktopHostInfo info = host.performance_info();
        const JsonValue frame = data::parse_json(host.performance_frame_json());
        try {
            return host::BrowserModel::build(
                frame, info.logical_width, info.logical_height
            ).resolve(selector);
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "performance target '" + selector_label(selector) + "' did not resolve: " +
                error.what()
            );
        }
    }

    [[nodiscard]] host::BrowserBounds resolve_bounds(
        const host::Selector& selector
    ) {
        const DesktopHostInfo info = host.performance_info();
        const JsonValue frame = data::parse_json(host.performance_frame_json());
        try {
            return host::BrowserModel::build(
                frame, info.logical_width, info.logical_height
            ).resolve_bounds(selector);
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "performance target '" + selector_label(selector) +
                "' did not resolve to bounds: " + error.what()
            );
        }
    }

    void execute(
        const PerformanceStep& step,
        const bool measured,
        PhaseResult* phase,
        const std::uint32_t iteration
    ) {
        std::visit([&](const auto& value) { execute(value, measured, phase, iteration); }, step);
    }

    void execute(
        const PerformanceFramesStep& step,
        const bool measured,
        PhaseResult* phase,
        const std::uint32_t iteration
    ) {
        for (std::uint32_t index = 0U; index < step.count; ++index) {
            run_frame(measured, phase, iteration, "frame");
        }
    }

    void execute(
        const PerformanceMoveStep& step,
        const bool measured,
        PhaseResult* phase,
        const std::uint32_t iteration
    ) {
        const auto [x, y] = resolve(step.target);
        const double scale = host.scale();
        host.pointer(STRATA_INPUT_POINTER_MOVE, 0, x * scale, y * scale);
        run_frame(measured, phase, iteration, "move:" + selector_label(step.target));
    }

    void execute(
        const PerformanceClickStep& step,
        const bool measured,
        PhaseResult* phase,
        const std::uint32_t iteration
    ) {
        const auto [x, y] = resolve(step.target);
        const double scale = host.scale();
        host.pointer(STRATA_INPUT_POINTER_MOVE, 0, x * scale, y * scale);
        run_frame(measured, phase, iteration, "click-move:" + selector_label(step.target));
        host.pointer(STRATA_INPUT_POINTER_PRESS, 0, x * scale, y * scale);
        run_frame(measured, phase, iteration, "click-press:" + selector_label(step.target));
        host.pointer(STRATA_INPUT_POINTER_RELEASE, 0, x * scale, y * scale);
        run_frame(measured, phase, iteration, "click-release:" + selector_label(step.target));
    }

    void execute(
        const PerformanceDragStep& step,
        const bool measured,
        PhaseResult* phase,
        const std::uint32_t iteration
    ) {
        const host::BrowserBounds bounds = resolve_bounds(step.target);
        const double scale = host.scale();
        const double y = bounds.y + bounds.height * 0.5;
        const auto x = [&bounds](const double fraction) {
            return bounds.x + bounds.width * fraction;
        };
        host.pointer(
            STRATA_INPUT_POINTER_MOVE,
            0,
            x(step.from_fraction) * scale,
            y * scale
        );
        run_frame(measured, phase, iteration, "drag-hover:" + selector_label(step.target));
        host.pointer(
            STRATA_INPUT_POINTER_PRESS,
            0,
            x(step.from_fraction) * scale,
            y * scale
        );
        run_frame(measured, phase, iteration, "drag-press:" + selector_label(step.target));
        for (std::uint32_t move = 0U; move < step.moves; ++move) {
            const double fraction = move % 2U == 0U
                ? step.to_fraction
                : step.from_fraction;
            host.pointer(
                STRATA_INPUT_POINTER_MOVE,
                0,
                x(fraction) * scale,
                y * scale
            );
            run_frame(
                measured,
                phase,
                iteration,
                "drag-move:" + selector_label(step.target)
            );
        }
        const double release_fraction = (step.moves - 1U) % 2U == 0U
            ? step.to_fraction
            : step.from_fraction;
        host.pointer(
            STRATA_INPUT_POINTER_RELEASE,
            0,
            x(release_fraction) * scale,
            y * scale
        );
        run_frame(measured, phase, iteration, "drag-release:" + selector_label(step.target));
    }

    void execute(
        const PerformanceScrollStep& step,
        const bool measured,
        PhaseResult* phase,
        const std::uint32_t iteration
    ) {
        const auto [x, y] = resolve(step.target);
        const double scale = host.scale();
        host.scroll(x * scale, y * scale, step.delta_x, step.delta_y);
        run_frame(measured, phase, iteration, "scroll:" + selector_label(step.target));
    }

    void execute(
        const PerformanceKeyStep& step,
        const bool measured,
        PhaseResult* phase,
        const std::uint32_t iteration
    ) {
        host.key(virtual_key(step.key));
        run_frame(measured, phase, iteration, "key:" + step.key);
    }

    void execute_steps(
        const std::vector<PerformanceStep>& steps,
        const bool measured,
        PhaseResult* phase,
        const std::uint32_t iteration
    ) {
        for (const PerformanceStep& step : steps) execute(step, measured, phase, iteration);
    }

    [[nodiscard]] static std::vector<std::int64_t> timing(
        const std::vector<FrameRecord>& frames,
        const std::int64_t DesktopFrameSample::* member
    ) {
        std::vector<std::int64_t> result;
        result.reserve(frames.size());
        for (const FrameRecord& frame : frames) result.push_back(frame.sample.*member);
        return result;
    }

    [[nodiscard]] JsonValue frame_json(const FrameRecord& frame) const {
        return object({
            {"batches", JsonValue(static_cast<std::int64_t>(frame.sample.batches))},
            {"blurPasses", JsonValue(static_cast<std::int64_t>(frame.sample.blur_passes))},
            {"coreMillis", JsonValue(millis(frame.sample.core_nanos))},
            {"drawCalls", JsonValue(static_cast<std::int64_t>(frame.sample.draw_calls))},
            {"emittedEvents", JsonValue(static_cast<std::int64_t>(frame.sample.emitted_events))},
            {"frameIndex", JsonValue(static_cast<std::int64_t>(frame.sample.frame_index))},
            {"frameMillis", JsonValue(millis(frame.sample.frame_time_nanos))},
            {"hadDraws", JsonValue(frame.sample.had_draws)},
            {"inputEvents", JsonValue(static_cast<std::int64_t>(frame.sample.input_events))},
            {"iteration", JsonValue(static_cast<std::int64_t>(frame.iteration))},
            {"operation", JsonValue(frame.operation)},
            {"packetBytes", JsonValue(static_cast<std::int64_t>(frame.sample.packet_bytes))},
            {"presented", JsonValue(frame.sample.presented)},
            {"presentMillis", JsonValue(millis(frame.sample.present_nanos))},
            {"renderCommands", JsonValue(static_cast<std::int64_t>(frame.sample.render_commands))},
            {"sequence", JsonValue(static_cast<std::int64_t>(frame.sequence))},
            {"submitMillis", JsonValue(millis(frame.sample.submit_nanos))},
            {"toolingMillis", JsonValue(millis(frame.sample.tooling_nanos))},
            {"totalMillis", JsonValue(millis(frame.sample.total_nanos))},
            {"valid", JsonValue(frame.valid)},
            {"vertices", JsonValue(static_cast<std::int64_t>(frame.sample.vertices))},
        });
    }

    [[nodiscard]] JsonValue phase_json(const PhaseResult& result) const {
        const Distribution total = distribution(timing(result.frames, &DesktopFrameSample::total_nanos));
        const double threshold = std::max(
            scenario.spike_floor_millis,
            millis(total.p50) * scenario.spike_multiplier
        );
        std::vector<const FrameRecord*> spikes;
        for (const FrameRecord& frame : result.frames) {
            if (millis(frame.sample.total_nanos) > threshold) spikes.push_back(&frame);
        }
        std::ranges::sort(spikes, [](const FrameRecord* left, const FrameRecord* right) {
            return left->sample.total_nanos > right->sample.total_nanos;
        });
        if (spikes.size() > scenario.maximum_spikes) spikes.resize(scenario.maximum_spikes);
        std::vector<JsonValue> spike_values;
        for (const FrameRecord* frame : spikes) spike_values.push_back(frame_json(*frame));
        std::vector<JsonValue> frame_values;
        frame_values.reserve(result.frames.size());
        std::map<std::string, std::vector<std::int64_t>, std::less<>> operations;
        for (const FrameRecord& frame : result.frames) {
            frame_values.push_back(frame_json(frame));
            operations[frame.operation].push_back(frame.sample.total_nanos);
        }
        std::vector<JsonValue> operation_values;
        operation_values.reserve(operations.size());
        for (auto& [name, values] : operations) {
            operation_values.push_back(object({
                {"name", JsonValue(name)},
                {"total", distribution_json(distribution(std::move(values)))},
            }));
        }
        return object({
            {"core", distribution_json(distribution(timing(result.frames, &DesktopFrameSample::core_nanos)))},
            {"frames", array(std::move(frame_values))},
            {"iterations", JsonValue(static_cast<std::int64_t>(result.phase.iterations))},
            {"name", JsonValue(result.phase.name)},
            {"operations", array(std::move(operation_values))},
            {"present", distribution_json(distribution(timing(result.frames, &DesktopFrameSample::present_nanos)))},
            {"spikeThresholdMillis", JsonValue(threshold)},
            {"spikes", array(std::move(spike_values))},
            {"submit", distribution_json(distribution(timing(result.frames, &DesktopFrameSample::submit_nanos)))},
            {"tooling", distribution_json(distribution(timing(result.frames, &DesktopFrameSample::tooling_nanos)))},
            {"total", distribution_json(total)},
            {"warmupFrames", JsonValue(static_cast<std::int64_t>(result.phase.warmup_frames))},
            {"warmupIterations", JsonValue(static_cast<std::int64_t>(result.phase.warmup_iterations))},
        });
    }

    [[nodiscard]] JsonValue environment_json() const {
        const DesktopHostInfo& info = host_info;
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof(monitor);
        const bool has_monitor = GetMonitorInfoW(expected_monitor, &monitor) != 0;
        DEVMODEW display{};
        display.dmSize = sizeof(display);
        static_cast<void>(EnumDisplaySettingsW(
            has_monitor ? monitor.szDevice : nullptr, ENUM_CURRENT_SETTINGS, &display
        ));
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        SYSTEM_POWER_STATUS power{};
        const bool has_power = GetSystemPowerStatus(&power) != 0;
        PROCESS_POWER_THROTTLING_STATE throttling{};
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        const bool has_throttling = GetProcessInformation(
            GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling)
        ) != 0;
        MEMORY_PRIORITY_INFORMATION memory_priority{};
        const bool has_memory_priority = GetProcessInformation(
            GetCurrentProcess(), ProcessMemoryPriority,
            &memory_priority, sizeof(memory_priority)
        ) != 0;
        return object({
            {"adapter", object({
                {"dedicatedVideoMemoryBytes", JsonValue(static_cast<std::int64_t>(
                    std::min<std::uint64_t>(info.dedicated_video_memory,
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                ))},
                {"deviceId", JsonValue(static_cast<std::int64_t>(info.device_id))},
                {"driverVersion", JsonValue(info.driver_version)},
                {"name", JsonValue(info.adapter)},
                {"vendorId", JsonValue(static_cast<std::int64_t>(info.vendor_id))},
            })},
            {"cpu", object({
                {"logicalProcessors", JsonValue(static_cast<std::int64_t>(system.dwNumberOfProcessors))},
                {"name", JsonValue(environment_text(L"PROCESSOR_IDENTIFIER"))},
            })},
            {"display", object({
                {"clientHeight", JsonValue(static_cast<std::int64_t>(info.framebuffer_height))},
                {"clientWidth", JsonValue(static_cast<std::int64_t>(info.framebuffer_width))},
                {"dpi", JsonValue(static_cast<std::int64_t>(expected_dpi))},
                {"dpiScale", JsonValue(info.scale)},
                {"monitorHeight", JsonValue(static_cast<std::int64_t>(
                    has_monitor ? monitor.rcMonitor.bottom - monitor.rcMonitor.top : 0
                ))},
                {"monitorWidth", JsonValue(static_cast<std::int64_t>(
                    has_monitor ? monitor.rcMonitor.right - monitor.rcMonitor.left : 0
                ))},
                {"refreshHz", JsonValue(static_cast<std::int64_t>(display.dmDisplayFrequency))},
            })},
            {"drawsRequired", JsonValue(scenario.require_draws)},
            {"foregroundRequired", JsonValue(scenario.require_foreground)},
            {"gpuCompletionSynchronized", JsonValue(false)},
            {"visibleRequired", JsonValue(scenario.require_visible)},
            {"power", object({
                {"acLine", has_power ? JsonValue(static_cast<std::int64_t>(power.ACLineStatus)) : JsonValue{}},
                {"batteryPercent", has_power ? JsonValue(static_cast<std::int64_t>(power.BatteryLifePercent)) : JsonValue{}},
            })},
            {"priority", object({
                {"class", JsonValue(priority_name(GetPriorityClass(GetCurrentProcess())))},
                {"executionSpeedThrottled", has_throttling
                    ? JsonValue(
                        (throttling.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0U &&
                        (throttling.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0U
                    )
                    : JsonValue{}},
                {"memory", has_memory_priority
                    ? JsonValue(static_cast<std::int64_t>(memory_priority.MemoryPriority))
                    : JsonValue{}},
                {"thread", JsonValue(static_cast<std::int64_t>(
                    GetThreadPriority(GetCurrentThread())
                ))},
            })},
            {"priorityClass", JsonValue(priority_name(GetPriorityClass(GetCurrentProcess())))},
            {"profilerSampling", JsonValue(false)},
            {"renderer", JsonValue("d3d11-swap-chain")},
            {"uncapped", JsonValue(true)},
            {"vsync", JsonValue(info.vsync)},
            {"windowVisible", JsonValue(IsWindowVisible(window) != 0)},
        });
    }

    [[nodiscard]] JsonValue report_json(const JsonValue& comparison) const {
        std::vector<JsonValue> phases_json;
        std::vector<std::int64_t> all_total;
        std::vector<std::int64_t> all_core;
        std::vector<std::int64_t> all_submit;
        std::vector<std::int64_t> all_present;
        for (const PhaseResult& phase : phases) {
            phases_json.push_back(phase_json(phase));
            const auto append = [&phase](
                                    std::vector<std::int64_t>& target,
                                    const std::int64_t DesktopFrameSample::* member
                                ) {
                for (const FrameRecord& frame : phase.frames) target.push_back(frame.sample.*member);
            };
            append(all_total, &DesktopFrameSample::total_nanos);
            append(all_core, &DesktopFrameSample::core_nanos);
            append(all_submit, &DesktopFrameSample::submit_nanos);
            append(all_present, &DesktopFrameSample::present_nanos);
        }
        std::vector<JsonValue> reasons;
        for (const std::string& reason : invalid_reasons) reasons.emplace_back(reason);
        return object({
            {"comparison", comparison},
            {"environment", environment_json()},
            {"generatedAtUtc", JsonValue(timestamp_utc())},
            {"invalidReasons", array(std::move(reasons))},
            {"phases", array(std::move(phases_json))},
            {"protocol", JsonValue("strata.desktop.performance")},
            {"scenario", JsonValue(scenario.name)},
            {"startup", object({
                {"firstFrameMillis", JsonValue(millis(first_frame.total_nanos))},
                {"hostCreateMillis", JsonValue(millis(startup.host_create_nanos))},
                {"readyMillis", JsonValue(millis(ready_nanos))},
                {"setupMillis", JsonValue(millis(setup_nanos))},
                {"windowCreateMillis", JsonValue(millis(startup.window_create_nanos))},
            })},
            {"summary", object({
                {"core", distribution_json(distribution(std::move(all_core)))},
                {"present", distribution_json(distribution(std::move(all_present)))},
                {"submit", distribution_json(distribution(std::move(all_submit)))},
                {"total", distribution_json(distribution(std::move(all_total)))},
            })},
            {"valid", JsonValue(invalid_reasons.empty())},
            {"version", JsonValue(std::int64_t{1})},
            {"workloadFingerprint", JsonValue(scenario.workload_fingerprint)},
        });
    }

    [[nodiscard]] JsonValue compare_with_baseline(const JsonValue& current) {
        std::ifstream input(baseline_report, std::ios::binary);
        if (!input) {
            throw std::runtime_error(
                "could not open performance baseline: " + baseline_report.string()
            );
        }
        const std::string source{std::istreambuf_iterator<char>(input), {}};
        const JsonValue baseline = data::parse_json(source);
        const auto string_field = [](const JsonValue* value, const std::string_view field) {
            const JsonValue* nested = value != nullptr ? value->find(field) : nullptr;
            return nested != nullptr && nested->string() != nullptr
                ? *nested->string() : std::string{};
        };
        const auto bool_field = [](const JsonValue* value, const std::string_view field)
            -> std::optional<bool> {
            const JsonValue* nested = value != nullptr ? value->find(field) : nullptr;
            if (nested == nullptr || nested->boolean() == nullptr) return std::nullopt;
            return *nested->boolean();
        };
        const auto number_field = [](const JsonValue* value, const std::string_view field) {
            const JsonValue* nested = value != nullptr ? value->find(field) : nullptr;
            if (nested != nullptr && nested->number() != nullptr) return *nested->number();
            if (nested != nullptr && nested->integer() != nullptr) {
                return static_cast<double>(*nested->integer());
            }
            return std::numeric_limits<double>::quiet_NaN();
        };
        const auto integer_field = [](const JsonValue* value, const std::string_view field)
            -> std::optional<std::int64_t> {
            const JsonValue* nested = value != nullptr ? value->find(field) : nullptr;
            if (nested == nullptr || nested->integer() == nullptr) return std::nullopt;
            return *nested->integer();
        };
        std::vector<JsonValue> reasons;
        bool comparable = true;
        if (string_field(&baseline, "protocol") != "strata.desktop.performance" ||
            integer_field(&baseline, "version") != std::optional<std::int64_t>{1} ||
            bool_field(&baseline, "valid") != std::optional<bool>{true}) {
            reasons.emplace_back("Baseline is not a valid version-1 Strata desktop performance report.");
            comparable = false;
        }
        if (string_field(&baseline, "scenario") != scenario.name ||
            string_field(&baseline, "workloadFingerprint") != scenario.workload_fingerprint) {
            reasons.emplace_back("Baseline was produced by a different performance workload.");
            comparable = false;
        }
        const JsonValue* current_environment = current.find("environment");
        const JsonValue* baseline_environment = baseline.find("environment");
        const JsonValue* current_adapter = current_environment != nullptr
            ? current_environment->find("adapter") : nullptr;
        const JsonValue* baseline_adapter = baseline_environment != nullptr
            ? baseline_environment->find("adapter") : nullptr;
        const JsonValue* current_display = current_environment != nullptr
            ? current_environment->find("display") : nullptr;
        const JsonValue* baseline_display = baseline_environment != nullptr
            ? baseline_environment->find("display") : nullptr;
        const auto require_same = [&reasons, &comparable](
                                      const bool same,
                                      const std::string_view message
                                  ) {
            if (same) return;
            reasons.emplace_back(std::string(message));
            comparable = false;
        };
        require_same(
            string_field(current_adapter, "name") == string_field(baseline_adapter, "name"),
            "GPU adapter differs from the baseline."
        );
        require_same(
            string_field(current_adapter, "driverVersion") ==
                string_field(baseline_adapter, "driverVersion"),
            "GPU driver differs from the baseline."
        );
        require_same(
            number_field(current_display, "clientWidth") ==
                    number_field(baseline_display, "clientWidth") &&
                number_field(current_display, "clientHeight") ==
                    number_field(baseline_display, "clientHeight"),
            "Client resolution differs from the baseline."
        );
        require_same(
            number_field(current_display, "dpi") == number_field(baseline_display, "dpi") &&
                number_field(current_display, "refreshHz") ==
                    number_field(baseline_display, "refreshHz") &&
                number_field(current_display, "monitorWidth") ==
                    number_field(baseline_display, "monitorWidth") &&
                number_field(current_display, "monitorHeight") ==
                    number_field(baseline_display, "monitorHeight"),
            "Display DPI, refresh rate, or monitor mode differs from the baseline."
        );
        require_same(
            bool_field(current_environment, "vsync").has_value() &&
                bool_field(current_environment, "vsync") ==
                    bool_field(baseline_environment, "vsync"),
            "VSync mode differs from the baseline."
        );
        require_same(
            string_field(current_environment, "priorityClass") ==
                string_field(baseline_environment, "priorityClass"),
            "Process priority differs from the baseline."
        );

        const auto phase_named = [](const JsonValue& report, const std::string_view name)
            -> const JsonValue* {
            const JsonValue* phases = report.find("phases");
            if (phases == nullptr || phases->array() == nullptr) return nullptr;
            for (const JsonValue& phase : *phases->array()) {
                const JsonValue* candidate = phase.find("name");
                if (candidate != nullptr && candidate->string() != nullptr &&
                    *candidate->string() == name) {
                    return &phase;
                }
            }
            return nullptr;
        };
        const auto metric = [&number_field](
                                const JsonValue* phase,
                                const std::string_view name
                            ) {
            return number_field(phase != nullptr ? phase->find("total") : nullptr, name);
        };
        const auto change = [](const double current_value, const double baseline_value) {
            return (current_value / baseline_value - 1.0) * 100.0;
        };
        std::vector<JsonValue> phase_values;
        bool regression = false;
        const JsonValue* current_phases = current.find("phases");
        const JsonValue* baseline_phases = baseline.find("phases");
        const std::size_t current_phase_count =
            current_phases != nullptr && current_phases->array() != nullptr
                ? current_phases->array()->size() : 0U;
        const std::size_t baseline_phase_count =
            baseline_phases != nullptr && baseline_phases->array() != nullptr
                ? baseline_phases->array()->size() : 0U;
        require_same(
            current_phase_count > 0U && current_phase_count == baseline_phase_count,
            "Baseline phase set differs from the current workload."
        );
        if (current_phases != nullptr && current_phases->array() != nullptr) {
            for (const JsonValue& phase : *current_phases->array()) {
                const std::string name = string_field(&phase, "name");
                const JsonValue* previous = phase_named(baseline, name);
                if (previous == nullptr) {
                    reasons.emplace_back("Baseline is missing phase '" + name + "'.");
                    comparable = false;
                    continue;
                }
                const double current_average = metric(&phase, "averageMillis");
                const double baseline_average = metric(previous, "averageMillis");
                const double current_p95 = metric(&phase, "p95Millis");
                const double baseline_p95 = metric(previous, "p95Millis");
                const double current_p99 = metric(&phase, "p99Millis");
                const double baseline_p99 = metric(previous, "p99Millis");
                const double current_count = metric(&phase, "count");
                const double baseline_count = metric(previous, "count");
                if (!std::isfinite(baseline_average) || baseline_average <= 0.0 ||
                    !std::isfinite(baseline_p95) || baseline_p95 <= 0.0 ||
                    !std::isfinite(baseline_p99) || baseline_p99 <= 0.0 ||
                    !std::isfinite(baseline_count) || baseline_count <= 0.0 ||
                    baseline_count != current_count) {
                    reasons.emplace_back(
                        "Baseline phase '" + name + "' has missing or incompatible metrics."
                    );
                    comparable = false;
                    continue;
                }
                const double average_change = change(current_average, baseline_average);
                const double p95_change = change(current_p95, baseline_p95);
                const double p99_change = change(current_p99, baseline_p99);
                const auto regressed = [this](
                                           const double change_percent,
                                           const double current_millis,
                                           const double baseline_millis,
                                           const double maximum_percent
                                       ) {
                    return change_percent > maximum_percent &&
                        current_millis - baseline_millis > scenario.minimum_regression_millis;
                };
                const bool phase_regression = regressed(
                        average_change, current_average, baseline_average,
                        scenario.maximum_average_regression_percent
                    ) || regressed(
                        p95_change, current_p95, baseline_p95,
                        scenario.maximum_p95_regression_percent
                    ) || regressed(
                        p99_change, current_p99, baseline_p99,
                        scenario.maximum_p99_regression_percent
                    );
                regression = regression || phase_regression;
                phase_values.push_back(object({
                    {"averageChangePercent", JsonValue(average_change)},
                    {"baselineAverageMillis", JsonValue(baseline_average)},
                    {"baselineP95Millis", JsonValue(baseline_p95)},
                    {"baselineP99Millis", JsonValue(baseline_p99)},
                    {"currentAverageMillis", JsonValue(current_average)},
                    {"currentP95Millis", JsonValue(current_p95)},
                    {"currentP99Millis", JsonValue(current_p99)},
                    {"name", JsonValue(name)},
                    {"p95ChangePercent", JsonValue(p95_change)},
                    {"p99ChangePercent", JsonValue(p99_change)},
                    {"regression", JsonValue(phase_regression)},
                }));
            }
        }
        comparison_failed = !comparable || regression;
        const std::string status = !comparable ? "incomparable"
            : regression ? "regression" : "pass";
        return object({
            {"baseline", JsonValue(baseline_report.string())},
            {"comparable", JsonValue(comparable)},
            {"phases", array(std::move(phase_values))},
            {"reasons", array(std::move(reasons))},
            {"regression", JsonValue(regression)},
            {"status", JsonValue(status)},
            {"thresholds", object({
                {"averagePercent", JsonValue(scenario.maximum_average_regression_percent)},
                {"p95Percent", JsonValue(scenario.maximum_p95_regression_percent)},
                {"p99Percent", JsonValue(scenario.maximum_p99_regression_percent)},
                {"minimumAbsoluteMillis", JsonValue(scenario.minimum_regression_millis)},
            })},
        });
    }

    [[nodiscard]] bool run(const PumpMessages& pump) {
        pump_messages = pump;
        try {
            const Clock::time_point setup_started = Clock::now();
            execute_steps(scenario.setup, false, nullptr, 0U);
            setup_nanos = nanos_since(setup_started);
            ready_nanos = nanos_since(startup.started_at);
            for (const PerformancePhase& phase : scenario.phases) {
                phases.emplace_back();
                PhaseResult& result = phases.back();
                result.phase = phase;
                for (std::uint32_t frame = 0U; frame < phase.warmup_frames; ++frame) {
                    run_frame(false, nullptr, 0U, "warmup-frame");
                }
                for (std::uint32_t iteration = 0U;
                     iteration < phase.warmup_iterations;
                     ++iteration) {
                    execute_steps(phase.steps, false, nullptr, iteration);
                }
                for (std::uint32_t iteration = 0U; iteration < phase.iterations; ++iteration) {
                    execute_steps(phase.steps, true, &result, iteration);
                }
            }
        } catch (const std::exception& error) {
            invalidate("Benchmark execution stopped: " + std::string(error.what()));
            if (ready_nanos == 0) ready_nanos = nanos_since(startup.started_at);
        }

        const JsonValue initial_report = report_json(JsonValue{});
        JsonValue comparison;
        if (!baseline_report.empty() && invalid_reasons.empty()) {
            try {
                comparison = compare_with_baseline(initial_report);
            } catch (const std::exception& error) {
                comparison_failed = true;
                comparison = object({
                    {"baseline", JsonValue(baseline_report.string())},
                    {"comparable", JsonValue(false)},
                    {"phases", array(std::vector<JsonValue>{})},
                    {"reasons", array({JsonValue(
                        "Could not compare baseline: " + std::string(error.what())
                    )})},
                    {"regression", JsonValue(false)},
                    {"status", JsonValue("incomparable")},
                });
            }
        }
        const JsonValue report = report_json(comparison);
        const std::string encoded = data::encode_canonical_json(report);
        write_file(output_root / "performance.json", encoded);
        write_file(output_root / "performance.html", html_report(encoded));
        const JsonValue* summary = report.find("summary");
        const JsonValue* total = summary != nullptr ? summary->find("total") : nullptr;
        const double average_fps = total != nullptr && total->find("averageFps") != nullptr &&
            total->find("averageFps")->number() != nullptr
            ? *total->find("averageFps")->number() : 0.0;
        const double p99 = total != nullptr && total->find("p99Millis") != nullptr &&
            total->find("p99Millis")->number() != nullptr
            ? *total->find("p99Millis")->number() : 0.0;
        const bool successful = invalid_reasons.empty() && !comparison_failed;
        std::string status = successful ? "VALID" : "INVALID";
        if (comparison_failed && comparison.find("status") != nullptr &&
            comparison.find("status")->string() != nullptr) {
            status = *comparison.find("status")->string() == "regression"
                ? "REGRESSION" : "INCOMPARABLE";
        }
        std::cout << std::fixed << std::setprecision(2)
                  << "STRATA DESKTOP PERFORMANCE " << status
                  << " avg=" << average_fps << " fps p99=" << p99 << " ms report="
                  << (output_root / "performance.html").string() << '\n';
        return successful;
    }

    HWND window = nullptr;
    Host& host;
    PerformanceScenario scenario;
    std::filesystem::path output_root;
    std::filesystem::path baseline_report;
    PerformanceStartup startup;
    DesktopHostInfo host_info;
    HMONITOR expected_monitor = nullptr;
    UINT expected_dpi = USER_DEFAULT_SCREEN_DPI;
    PumpMessages pump_messages;
    std::vector<std::string> invalid_reasons;
    std::vector<PhaseResult> phases;
    DesktopFrameSample first_frame;
    std::uint64_t sequence = 0U;
    std::int64_t setup_nanos = 0;
    std::int64_t ready_nanos = 0;
    bool comparison_failed = false;
};

PerformanceRunner::PerformanceRunner(
    HWND window,
    Host& host,
    PerformanceScenario scenario,
    std::filesystem::path output_root,
    std::filesystem::path baseline_report,
    PerformanceStartup startup
) : impl_(std::make_unique<Impl>(
        window, host, std::move(scenario), std::move(output_root),
        std::move(baseline_report), startup
    )) {}

PerformanceRunner::~PerformanceRunner() = default;

bool PerformanceRunner::run(const PumpMessages& pump_messages) {
    return impl_->run(pump_messages);
}

} // namespace strata::desktop
