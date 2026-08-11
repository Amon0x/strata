#include "host/browser_model.hpp"
#include "headless/scenario.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#include "data/json.hpp"

namespace {

void check(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] strata::data::JsonValue frame(const bool row_available) {
    const std::string subtargets = row_available
        ? R"([
            {
              "bounds":{"x":1084,"y":194,"width":28,"height":26},
              "commandId":null,"detached":false,"enabled":true,"id":"$control",
              "index":0,"kind":"control","label":"","notificationId":null,"path":[]
            },
            {
              "bounds":{"x":920,"y":230,"width":192,"height":30},
              "commandId":null,"detached":true,"enabled":true,"id":"$menu/0",
              "index":0,"kind":"choice","label":"Set feature keybind",
              "notificationId":null,"path":[0]
            }
          ])"
        : R"([
            {
              "bounds":{"x":1084,"y":194,"width":28,"height":26},
              "commandId":null,"detached":false,"enabled":true,"id":"$control",
              "index":0,"kind":"control","label":"","notificationId":null,"path":[]
            }
          ])";
    return strata::data::parse_json(
        R"({
          "inspection":{"root":{
            "structuralPath":"/",
            "hitBounds":{"x":0,"y":0,"width":1280,"height":800},
            "clip":{"x":0,"y":0,"width":1280,"height":800},
            "subtargets":[],
            "children":[{
              "structuralPath":"/0",
              "hitBounds":{"x":1084,"y":194,"width":28,"height":26},
              "subtargets":)" + subtargets + R"(,
              "children":[]
            }]
          }},
          "semantics":{"root":{
            "structuralPath":"/","key":null,"name":"","role":"group",
            "actions":[],"state":{},"children":[{
              "structuralPath":"/0","key":"feature.menu","name":"Feature","role":"menu",
              "actions":["collapse","focus"],"state":{"expanded":true},"children":[{
                "structuralPath":"/0/2100000","key":"feature.menu",
                "name":"Set feature keybind","role":"menu_item",
                "actions":["activate"],"state":{"disabled":false},"virtualCommandId":null,
                "virtualIndex":null,"virtualNotificationId":null,"children":[]
              }]
            }]
          }}
        })"
    );
}

[[nodiscard]] const strata::host::BrowserElement& element(
    const strata::host::BrowserModel& model,
    const std::string& path
) {
    for (const strata::host::BrowserElement& candidate : model.elements()) {
        if (candidate.path == path) return candidate;
    }
    throw std::runtime_error("browser element was not found");
}

void virtual_menu_rows_require_exact_geometry() {
    const strata::host::BrowserModel open =
        strata::host::BrowserModel::build(frame(true), 1280.0, 800.0);
    const strata::host::BrowserElement& row = element(open, "/0/2100000");
    check(
        row.hit_bounds.has_value() &&
            row.hit_bounds->x == 920.0 &&
            row.hit_bounds->y == 230.0 &&
            row.hit_bounds->width == 192.0 &&
            row.hit_bounds->height == 30.0,
        "virtual menu row inherited or joined the trigger bounds"
    );

    const strata::host::BrowserModel collapsed =
        strata::host::BrowserModel::build(frame(false), 1280.0, 800.0);
    check(
        !element(collapsed, "/0/2100000").hit_bounds.has_value(),
        "unavailable virtual menu row inherited the control subtarget"
    );
    bool rejected = false;
    try {
        static_cast<void>(collapsed.resolve(strata::host::Selector{
            .name = "Set feature keybind",
            .role = "menu_item",
        }));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "selector resolved a collapsed virtual menu row");
}

void indexed_virtual_controls_keep_index_geometry() {
    const strata::data::JsonValue value = strata::data::parse_json(R"({
      "inspection":{"root":{
        "structuralPath":"/","hitBounds":{"x":0,"y":0,"width":400,"height":240},
        "clip":{"x":0,"y":0,"width":400,"height":240},"subtargets":[],"children":[{
          "structuralPath":"/0","hitBounds":{"x":20,"y":20,"width":140,"height":30},
          "subtargets":[
            {
              "bounds":{"x":20,"y":20,"width":140,"height":30},
              "commandId":null,"detached":false,"enabled":true,"id":"$control",
              "index":0,"kind":"control","label":"","notificationId":null,"path":[]
            },
            {
              "bounds":{"x":20,"y":54,"width":220,"height":28},
              "commandId":null,"detached":true,"enabled":true,"id":"fast",
              "index":0,"kind":"choice","label":"Fast","notificationId":null,"path":[]
            }
          ],"children":[]
        }]
      }},
      "semantics":{"root":{
        "structuralPath":"/","key":null,"name":"","role":"group","actions":[],"state":{},
        "children":[{
          "structuralPath":"/0","key":"quality","name":"Quality","role":"combo_box",
          "actions":["collapse","focus"],"state":{"expanded":true},"children":[{
            "structuralPath":"/0/2000000","key":"quality","name":"Fast","role":"list_item",
            "actions":["activate"],"state":{},"virtualCommandId":null,
            "virtualIndex":0,"virtualNotificationId":null,"children":[]
          }]
        }]
      }}
    })");
    const strata::host::BrowserModel model =
        strata::host::BrowserModel::build(value, 400.0, 240.0);
    const strata::host::BrowserElement& option = element(model, "/0/2000000");
    check(
        option.hit_bounds.has_value() &&
            option.hit_bounds->x == 20.0 &&
            option.hit_bounds->y == 54.0 &&
            option.hit_bounds->width == 220.0 &&
            option.hit_bounds->height == 28.0,
        "indexed virtual choice lost its empty-path subtarget geometry"
    );

    const strata::data::JsonValue collapsed_value = strata::data::parse_json(R"({
      "inspection":{"root":{
        "structuralPath":"/","hitBounds":{"x":0,"y":0,"width":400,"height":240},
        "clip":{"x":0,"y":0,"width":400,"height":240},"subtargets":[],"children":[{
          "structuralPath":"/0","hitBounds":{"x":20,"y":20,"width":140,"height":30},
          "subtargets":[{
            "bounds":{"x":20,"y":20,"width":140,"height":30},
            "commandId":null,"detached":false,"enabled":true,"id":"$control",
            "index":0,"kind":"control","label":"","notificationId":null,"path":[]
          }],"children":[]
        }]
      }},
      "semantics":{"root":{
        "structuralPath":"/","key":null,"name":"","role":"group","actions":[],"state":{},
        "children":[{
          "structuralPath":"/0","key":"quality","name":"Quality","role":"combo_box",
          "actions":["expand","focus"],"state":{"expanded":false},"children":[{
            "structuralPath":"/0/2000000","key":"quality","name":"Fast","role":"list_item",
            "actions":["activate"],"state":{},"virtualCommandId":null,
            "virtualIndex":0,"virtualNotificationId":null,"children":[]
          }]
        }]
      }}
    })");
    const strata::host::BrowserModel collapsed =
        strata::host::BrowserModel::build(collapsed_value, 400.0, 240.0);
    check(
        !element(collapsed, "/0/2000000").hit_bounds.has_value(),
        "collapsed indexed choice inherited its owner's index-zero control bounds"
    );
}

void click_steps_accept_secondary_buttons() {
    const strata::headless::ScenarioStep parsed =
        strata::headless::parse_scenario_step(strata::data::parse_json(
            R"({"click":{"key":"context.owner","button":"right"}})"
        ));
    const auto* click = std::get_if<strata::headless::ClickStep>(&parsed);
    check(
        click != nullptr && click->target.key == std::optional<std::string>("context.owner") &&
            click->button == 1,
        "headless click did not preserve the configured secondary button"
    );
}

} // namespace

int strata_test_browser_model() {
    try {
        virtual_menu_rows_require_exact_geometry();
        indexed_virtual_controls_keep_index_geometry();
        click_steps_accept_secondary_buttons();
        std::cout << "strata_browser_model_tests: exact virtual geometry and buttons OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_browser_model_tests: " << error.what() << '\n';
        return 1;
    }
}
