// How far a change reaches: a write, a publish or a retained value describes again only the
// instances that read it, and their callers take the new output without being described.

#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/compile.hpp"
#include "data/json.hpp"
#include "runtime/application.hpp"
#include "ui/description.hpp"
#include "ui/tree.hpp"

namespace {

using namespace strata;

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void activate(runtime::ApplicationContext& application, const std::string_view name,
              const std::string& source) {
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    const runtime::ActivationResult activation = application.compile_and_activate(
        compiler::ModuleSource{std::string(name) + ".strata", source}, no_imports, 0U);
    std::string failure = "no diagnostic";
    if (!activation.diagnostics.empty())
        failure =
            activation.diagnostics.front().code + ": " + activation.diagnostics.front().message;
    check(activation.activated(), std::string(name) + " fixture did not activate: " + failure);
}

/** The described node with a key, among eagerly described children only (no row is realized). */
[[nodiscard]] std::shared_ptr<const ui::DescriptionNode>
find(const std::shared_ptr<const ui::DescriptionNode>& node, const std::string_view key) {
    if (node == nullptr)
        return nullptr;
    if (node->key.has_value() && *node->key == key)
        return node;
    if (dynamic_cast<const ui::EagerDescriptionChildren*>(node->children.get()) == nullptr)
        return nullptr;
    for (std::size_t index = 0U; index < node->children->size(); ++index) {
        if (auto found = find(node->children->at(index), key); found != nullptr)
            return found;
    }
    return nullptr;
}

[[nodiscard]] std::string text(const std::shared_ptr<const ui::DescriptionNode>& node) {
    if (node == nullptr)
        return "<missing>";
    const auto property = node->properties.find("text");
    if (property == node->properties.end() || property->second.value() == nullptr ||
        property->second.value()->string() == nullptr) {
        return "<no text>";
    }
    return *property->second.value()->string();
}

void write_state(runtime::ApplicationContext& application, const std::string_view scope,
                 const std::string_view name, runtime::Value value) {
    const std::optional<runtime::StateScopeResolution> resolved =
        application.resolve_state_scope(scope, name);
    check(resolved.has_value() && resolved->declaration != nullptr,
          "state '" + std::string(name) + "' was not bound in '" + std::string(scope) + "'");
    const runtime::Value* const current = application.state().find(resolved->address);
    check(current != nullptr &&
              application.state().write(resolved->address,
                                        runtime::StateSlot{std::string(name),
                                                           resolved->declaration->type_id, *current,
                                                           resolved->declaration_scope},
                                        std::move(value)),
          "state '" + std::string(name) + "' did not change");
}

[[nodiscard]] std::shared_ptr<const runtime::ApplicationBundle>
host_bundle(const std::string_view registry, const std::string_view host) {
    const data::JsonValue schemas = data::parse_json(
        R"({"extensionPackages":[],"widgets":{"registry":")" + std::string(registry) +
        R"(","required":[],"definitions":[]},"actions":{"registry":")" + std::string(registry) +
        R"(","required":[],"definitions":[]},"host":)" + std::string(host) + "}");
    return runtime::ApplicationBundle::create(&schemas);
}

[[nodiscard]] std::string value_root(const std::string_view name) {
    return R"({"path":")" + std::string(name) +
           R"(","nullable":false,"type":{"kind":"object","allowUnknownFields":false,"valueNullable":false,"fields":[{"name":"value","type":{"kind":"string"},"required":true,"nullable":false}]}})";
}

void test_a_state_write_describes_only_the_instance_that_declares_it() {
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("invalidation-state", bundle);
    activate(application, "invalidation-state", R"(
component Counter(key: key, label: string) {
  state count = 0;
  Text(key: format("counter.{0}", label), text: format("{0}:{1}", label, count))
}
overlay Counters {
  state items = ["a", "b", "c", "d", "e", "f", "g", "h"];
  root Panel(key: "counters") {
    for item in items {
      Counter(key: item, label: item)
    }
  }
}
)");
    ui::DescriptionBuilder builder(application);
    const ui::DescriptionBuildResult first = builder.build(runtime::LayerRole::overlay, "Counters");
    check(first.diagnostics.empty() && first.described_instances == 9U,
          "the counter fixture did not describe its layer and eight counters");
    const std::shared_ptr<const ui::DescriptionNode> b = find(first.root, "counter.b");
    check(b != nullptr && b->materialization_key.has_value(),
          "a counter that starts a loop item was not named by its item");

    write_state(application, b->state_scope, "count", runtime::Value(1.0));
    const ui::DescriptionBuildResult second =
        builder.build(runtime::LayerRole::overlay, "Counters");
    check(second.described_instances == 1U && second.described_nodes == 1U,
          "a counter's state write described more than that counter: instances=" +
              std::to_string(second.described_instances) +
              " nodes=" + std::to_string(second.described_nodes));
    const std::shared_ptr<const ui::DescriptionNode> b_after = find(second.root, "counter.b");
    check(text(b_after) == "b:1" && b_after->materialization_key == b->materialization_key,
          "the counter that read the state lost its value or its loop item's name");
    for (const std::string_view key : {"counter.a", "counter.c", "counter.h"}) {
        check(find(second.root, key) == find(first.root, key),
              "a counter that did not read the state was described again");
    }
    const std::shared_ptr<const ui::DescriptionNode> panel = find(second.root, "counters");
    check(panel != find(first.root, "counters") &&
              panel->children_replaced_from.lock() == find(first.root, "counters"),
          "the layer holding the loop was described instead of taking the counter's output");

    const ui::DescriptionBuildResult settled =
        builder.build(runtime::LayerRole::overlay, "Counters");
    check(settled.root == second.root && settled.described_instances == 0U &&
              settled.evaluated_expressions == 0U,
          "a build with nothing changed described something");
}

void test_a_host_publish_describes_only_its_readers() {
    const auto bundle = host_bundle("invalidation-host",
                                    "[" + value_root("clock") + "," + value_root("weather") + "]");
    runtime::ApplicationContext application("invalidation-host", bundle);
    const auto publish = [&](const std::string_view root, const std::uint64_t generation,
                             const std::string_view value) {
        static_cast<void>(application.host().adopt(
            bundle->host_snapshot("invalidation-" + std::string(root), generation,
                                  data::parse_json("{\"" + std::string(root) + "\":{\"value\":\"" +
                                                   std::string(value) + "\"}}"))));
    };
    publish("clock", 1U, "1:00");
    publish("weather", 1U, "sun");
    activate(application, "invalidation-host", R"(
component Clock() { Text(key: "clock", text: clock.value) }
component Fixed() { Text(key: "fixed", text: "fixed") }
component Weather() { Text(key: "weather", text: weather.value) }
overlay Main {
  root Panel(key: "main") {
    Clock()
    Fixed()
    Weather()
  }
}
)");
    ui::DescriptionBuilder builder(application);
    const ui::DescriptionBuildResult first = builder.build(runtime::LayerRole::overlay, "Main");
    check(first.diagnostics.empty() && first.described_instances == 4U,
          "the host fixture did not describe its layer and three components");

    publish("clock", 2U, "1:01");
    const ui::DescriptionBuildResult ticked = builder.build(runtime::LayerRole::overlay, "Main");
    check(ticked.described_instances == 1U && text(find(ticked.root, "clock")) == "1:01" &&
              find(ticked.root, "weather") == find(first.root, "weather") &&
              find(ticked.root, "fixed") == find(first.root, "fixed"),
          "a clock publish described more than the component that read the clock");

    publish("clock", 3U, "1:01");
    const ui::DescriptionBuildResult same = builder.build(runtime::LayerRole::overlay, "Main");
    check(same.described_instances == 0U && same.root == ticked.root,
          "a publish of the same value described its reader again");

    publish("weather", 2U, "rain");
    const ui::DescriptionBuildResult rained = builder.build(runtime::LayerRole::overlay, "Main");
    check(rained.described_instances == 1U && text(find(rained.root, "weather")) == "rain" &&
              find(rained.root, "clock") == find(ticked.root, "clock"),
          "a weather publish described more than the component that read the weather");
}

void test_a_stale_row_is_described_alone() {
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("invalidation-rows", bundle);
    activate(application, "invalidation-rows", R"(
overlay Rows {
  root Repeater(key: "rows", estimatedItemExtent: 24) {
    for row in [{ id: "a" }, { id: "b" }, { id: "c" }] {
      Panel(key: row.id) {
        state open = false;
        Text(key: format("{0}.text", row.id), text: format("{0} {1}", row.id, open ? "open" : "closed"))
      }
    }
  }
}
)");
    ui::DescriptionBuilder builder(application);
    const ui::DescriptionBuildResult first = builder.build(runtime::LayerRole::overlay, "Rows");
    const std::shared_ptr<const ui::DescriptionNode> repeater = first.root->children->at(0U);
    // Realized rows stay held, as a retained tree holds them.
    std::vector<std::shared_ptr<const ui::DescriptionNode>> rows;
    for (std::size_t index = 0U; index < repeater->children->size(); ++index)
        rows.push_back(repeater->children->at(index));
    check(rows.size() == 3U && builder.telemetry().described_rows == 3U,
          "the row fixture did not realize its three rows");

    write_state(application, rows[1]->state_scope, "open", runtime::Value(true));
    const ui::DescriptionBuildResult second = builder.build(runtime::LayerRole::overlay, "Rows");
    const std::shared_ptr<const ui::DescriptionNode> reprovided = second.root->children->at(0U);
    check(second.described_instances == 0U && second.root != first.root &&
              reprovided->children != repeater->children &&
              reprovided->virtual_sequence == repeater->virtual_sequence,
          "a row's state write described the body holding the Repeater instead of the row");
    const std::uint64_t described = builder.telemetry().described_rows;
    check(reprovided->children->at(0U) == rows[0] && reprovided->children->at(2U) == rows[2],
          "rows that read nothing that changed were described again");
    const std::shared_ptr<const ui::DescriptionNode> opened = reprovided->children->at(1U);
    check(opened != rows[1] && text(find(opened, "b.text")) == "b open" &&
              builder.telemetry().described_rows == described + 1U,
          "the row that read the state was not described again, alone");
}

void test_rows_of_a_new_generation_keep_what_did_not_change() {
    const auto bundle = host_bundle("invalidation-generation", R"([{
        "path":"model","nullable":false,
        "type":{"kind":"object","allowUnknownFields":false,"valueNullable":false,"fields":[{
          "name":"rows","required":true,"nullable":false,
          "type":{"kind":"list","maximumItems":16,"elementNullable":false,"element":{
            "kind":"object","allowUnknownFields":false,"valueNullable":false,"fields":[
              {"name":"id","type":{"kind":"string"},"required":true,"nullable":false},
              {"name":"label","type":{"kind":"string"},"required":true,"nullable":false}
            ]}}
        }]}
      }])");
    runtime::ApplicationContext application("invalidation-generation", bundle);
    const auto publish = [&](const std::uint64_t generation, const std::string_view rows) {
        static_cast<void>(application.host().adopt(bundle->host_snapshot(
            "invalidation-model", generation,
            data::parse_json("{\"model\":{\"rows\":" + std::string(rows) + "}}"))));
    };
    publish(1U, R"([{"id":"a","label":"A"},{"id":"b","label":"B"},{"id":"c","label":"C"}])");
    activate(application, "invalidation-generation", R"(
overlay Hosted {
  root Repeater(key: "hosted", estimatedItemExtent: 24) {
    for row in model.rows {
      Text(key: row.id, text: row.label)
    }
  }
}
)");
    ui::DescriptionBuilder builder(application);
    const ui::DescriptionBuildResult first = builder.build(runtime::LayerRole::overlay, "Hosted");
    const std::shared_ptr<const ui::DescriptionNode> repeater = first.root->children->at(0U);
    std::vector<std::shared_ptr<const ui::DescriptionNode>> rows;
    for (std::size_t index = 0U; index < repeater->children->size(); ++index)
        rows.push_back(repeater->children->at(index));

    // A row is inserted before the others and one changes: the rest are handed out as they were.
    publish(
        2U,
        R"([{"id":"d","label":"D"},{"id":"a","label":"A"},{"id":"b","label":"B2"},{"id":"c","label":"C"}])");
    const ui::DescriptionBuildResult second = builder.build(runtime::LayerRole::overlay, "Hosted");
    const std::shared_ptr<const ui::DescriptionNode> regenerated = second.root->children->at(0U);
    check(regenerated->virtual_sequence != repeater->virtual_sequence &&
              regenerated->children->size() == 4U,
          "the published rows did not make a new generation");
    const std::uint64_t described = builder.telemetry().described_rows;
    const std::uint64_t reused = builder.telemetry().reused_rows;
    std::vector<std::shared_ptr<const ui::DescriptionNode>> next;
    for (std::size_t index = 0U; index < regenerated->children->size(); ++index)
        next.push_back(regenerated->children->at(index));
    check(next[1] == rows[0] && next[3] == rows[2],
          "rows whose item stayed the same were described again by the new generation");
    check(text(next[0]) == "D" && text(next[2]) == "B2" && next[2] != rows[1] &&
              builder.telemetry().described_rows == described + 2U &&
              builder.telemetry().reused_rows == reused + 2U,
          "the new and changed rows were not described, or more were");
}

void test_a_retained_write_beside_a_repeater_describes_only_its_reader() {
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("invalidation-retained", bundle);
    activate(application, "invalidation-retained", R"(
component Feed() {
  Panel(key: "feed") {
    Repeater(key: "feed.rows", estimatedItemExtent: 24) {
      for row in [{ id: "x" }, { id: "y" }] {
        Text(key: row.id, text: row.id)
      }
    }
  }
}
component Pane() {
  SplitPane(key: "pane.split", defaultRatio: 0.25, layout: { kind: "ROW", width: 120, height: 40 }) {
    Panel(key: "pane.left")
    Panel(key: "pane.right")
  }
}
overlay Main {
  root Panel(key: "main") {
    Feed()
    Pane()
  }
}
)");
    ui::DescriptionBuilder builder(application);
    ui::RetainedTree tree;
    ui::DescriptionBuildResult built = builder.build(runtime::LayerRole::overlay, "Main");
    for (int settle = 0; settle < 2; ++settle) {
        static_cast<void>(tree.reconcile(built.root));
        builder.set_retained_tree(&tree);
        built = builder.build(runtime::LayerRole::overlay, "Main");
    }
    static_cast<void>(tree.reconcile(built.root));
    const ui::RetainedNode* const split = tree.find_key("pane.split");
    const ui::RetainedNode* const feed = tree.find_key("feed.rows");
    check(split != nullptr && feed != nullptr, "the retained fixture was not reconciled");
    check(builder.observes_retained_value(*split, "strata.gesture.splitRatio") &&
              !builder.observes_retained_value(*feed, "strata.gesture.splitRatio"),
          "the retained-value query does not name exactly the node the description read");

    check(tree.set_retained_value(split->identity(), "strata.gesture.splitRatio",
                                  runtime::Value(0.75)),
          "the split ratio did not change");
    builder.set_retained_tree(&tree);
    const ui::DescriptionBuildResult moved = builder.build(runtime::LayerRole::overlay, "Main");
    check(moved.described_instances == 1U && find(moved.root, "feed") == find(built.root, "feed"),
          "a retained write beside a Repeater described the body holding the Repeater");
    const std::shared_ptr<const ui::DescriptionNode> left =
        find(moved.root, "pane.split")->children->at(0U);
    const runtime::Value* const layout = left->properties.at("$layout").value();
    const runtime::Value* const width = layout != nullptr ? layout->field("width") : nullptr;
    check(width != nullptr && width->field("weight") != nullptr &&
              width->field("weight")->number() != nullptr &&
              std::abs(*width->field("weight")->number() - 0.75) < 0.000'001,
          "the pane that read the ratio did not take its new value");
}

void test_a_slot_in_lazy_rows_takes_the_callers_content() {
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("invalidation-row-slots", bundle);
    activate(application, "invalidation-row-slots", R"(
component Rows() {
  Repeater(key: "slot.rows", estimatedItemExtent: 24) {
    for row in [{ id: "a" }, { id: "b" }] {
      Panel(key: row.id) {
        Text(key: format("{0}.label", row.id), text: row.id)
        Slot(name: "content")
      }
    }
  }
}
overlay Filled {
  state caption = "first";
  root Rows() {
    Text(key: "slot.caption", text: caption)
  }
}
)");
    ui::DescriptionBuilder builder(application);
    const ui::DescriptionBuildResult first = builder.build(runtime::LayerRole::overlay, "Filled");
    check(first.diagnostics.empty(), "the row slot fixture produced diagnostics");
    const std::shared_ptr<const ui::DescriptionNode> repeater = first.root->children->at(0U);
    // Describing the layer realizes no row: the slot is found in the row body itself.
    check(builder.telemetry().described_rows == 0U,
          "projecting content into a component realized its lazy rows");
    for (std::size_t index = 0U; index < repeater->children->size(); ++index) {
        check(text(find(repeater->children->at(index), "slot.caption")) == "first",
              "a slot inside a lazy row did not take the caller's content");
    }
    const std::uint64_t described = builder.telemetry().described_rows;

    // New content reaches the rows already realized when they are next realized.
    write_state(application, "overlay Filled", "caption", runtime::Value("second"));
    const ui::DescriptionBuildResult second = builder.build(runtime::LayerRole::overlay, "Filled");
    const std::shared_ptr<const ui::DescriptionNode> reprovided = second.root->children->at(0U);
    check(reprovided->children != repeater->children &&
              text(find(reprovided->children->at(0U), "slot.caption")) == "second" &&
              text(find(reprovided->children->at(1U), "slot.caption")) == "second" &&
              builder.telemetry().described_rows == described + 2U,
          "rows kept the content their caller no longer passes");
}

void test_a_failed_build_changes_nothing_outside_the_builder() {
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("invalidation-pure", bundle);
    application.configure_durable_store(runtime::DurableStoreAdapter{
        [](const std::string_view) -> std::optional<std::string> {
            return R"({"application":{"pure.count":"not a number"},"commands":{},"format":"strata.durable","shell":{},"version":1,"widgets":{}})";
        },
        [](const std::string_view, const std::string_view) {},
    });
    activate(application, "invalidation-pure", R"(
overlay Broken {
  state count: number = persisted("pure.count", 1);
  root Repeater(key: "pure.rows", estimatedItemExtent: 24) {
    for item in [{ label: "same" }, { label: "SAME" }] {
      Panel(key: upper(item.label))
    }
  }
}
overlay Fine {
  state count: number = 1;
  root Text(key: "pure.text", text: format("{0}", count))
}
)");
    check(application.durability().application_value("pure.count") != nullptr,
          "the purity fixture's durable value was not loaded");
    // Describing reads only: a build that fails after a state was declared stores no state,
    // binds no scope and discards no durable value.
    ui::DescriptionBuilder builder(application);
    bool rejected = false;
    try {
        static_cast<void>(builder.build(runtime::LayerRole::overlay, "Broken"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "the duplicate Repeater keys were not rejected");
    check(application.state().size() == 0U &&
              !application.resolve_state_scope("overlay Broken", "count").has_value() &&
              application.durability().application_value("pure.count") != nullptr,
          "a failed build stored state, bound a scope or discarded a durable value");

    // A build that succeeds commits what it described.
    const ui::DescriptionBuildResult fine = builder.build(runtime::LayerRole::overlay, "Fine");
    check(text(find(fine.root, "pure.text")) == "1" && application.state().size() == 1U &&
              application.resolve_state_scope("overlay Fine", "count").has_value(),
          "a successful build did not store its state or bind it");
}

} // namespace

int strata_test_description_invalidation() {
    try {
        test_a_state_write_describes_only_the_instance_that_declares_it();
        test_a_host_publish_describes_only_its_readers();
        test_a_stale_row_is_described_alone();
        test_rows_of_a_new_generation_keep_what_did_not_change();
        test_a_retained_write_beside_a_repeater_describes_only_its_reader();
        test_a_slot_in_lazy_rows_takes_the_callers_content();
        test_a_failed_build_changes_nothing_outside_the_builder();
        std::cout << "strata_description_invalidation_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_description_invalidation_tests: " << error.what() << '\n';
        return 1;
    }
}
