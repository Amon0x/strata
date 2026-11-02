// How far a change reaches: a write, a publish or a retained value describes again only the
// instances that read it, and their callers take the new output without being described.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/pending.hpp"
#include "base/task_pool.hpp"
#include "compiler/compile.hpp"
#include "data/json.hpp"
#include "resource/runtime_images.hpp"
#include "resource/svg_image.hpp"
#include "runtime/application.hpp"
#include "runtime/profiler.hpp"
#include "ui/description.hpp"
#include "ui/inspection.hpp"
#include "ui/surface.hpp"
#include "ui/svg_image.hpp"
#include "ui/tree.hpp"
#include "ui/widget/description.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/presentation.hpp"

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

/** Counter handlers must exercise their real closures without reading state or calling a host. */
class NoExternalScriptEffects final : public runtime::ScriptEffects {
  public:
    const runtime::Value* state(const runtime::LexicalStateBinding&) override {
        throw std::logic_error("counter unexpectedly read retained state");
    }
    std::optional<std::string> commit(runtime::ScriptTransaction) override {
        throw std::logic_error("counter unexpectedly committed retained state");
    }
    runtime::ScriptAnswer perform(const std::shared_ptr<const runtime::ActionValue>&) override {
        throw std::logic_error("counter unexpectedly performed an action");
    }
};

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

void test_retained_snapshots_disambiguate_keys_and_keep_prior_values() {
    struct Query {
        std::string_view key;
        std::string_view source;
        std::string_view scope;
        std::string_view type;
    };
    constexpr std::array queries{
        Query{"shared-key-for-retained-snapshot", "/first", "alpha", "Panel"},
        Query{"shared-key-for-retained-snapshot", "/first", "beta", "Panel"},
        Query{"shared-key-for-retained-snapshot", "/second", "alpha", "Panel"},
        Query{"shared-key-for-retained-snapshot", "/first", "alpha", "Button"},
        Query{"unique-key-for-retained-snapshot", "/first", "alpha", "Panel"},
    };
    const auto root = [&queries](const bool duplicate) {
        std::vector<std::shared_ptr<const ui::DescriptionNode>> branches;
        for (std::size_t index = 0U; index < queries.size() + (duplicate ? 1U : 0U); ++index) {
            const Query& query = queries[index % queries.size()];
            const auto child = ui::DescriptionNode::create(
                std::string(query.type), std::string(query.key), std::string(query.source),
                std::string(query.scope), {},
                std::make_shared<const ui::EagerDescriptionChildren>(
                    std::vector<std::shared_ptr<const ui::DescriptionNode>>{}));
            branches.push_back(ui::DescriptionNode::create(
                "Panel", "branch." + std::to_string(index), "/branch", "root", {},
                std::make_shared<const ui::EagerDescriptionChildren>(
                    std::vector<std::shared_ptr<const ui::DescriptionNode>>{child})));
        }
        return ui::DescriptionNode::create(
            "Panel", "root", "/root", "root", {},
            std::make_shared<const ui::EagerDescriptionChildren>(std::move(branches)));
    };
    const auto observed = [](const ui::RetainedDescriptionSnapshot& snapshot, const Query& query) {
        const auto* node = snapshot.find_key(query.key, query.source, query.scope, query.type);
        const runtime::Value* value = node != nullptr ? node->retained_value("value") : nullptr;
        check(value != nullptr && value->number() != nullptr,
              "retained query lost its scoped value");
        return *value->number();
    };
    std::shared_ptr<const ui::RetainedDescriptionSnapshot> saved;
    {
        ui::RetainedTree tree;
        static_cast<void>(tree.reconcile(root(false)));
        for (std::size_t index = 0U; index < queries.size(); ++index) {
            const auto* branch = tree.find_key("branch." + std::to_string(index));
            check(branch != nullptr &&
                      tree.set_retained_value(branch->children().front()->identity(), "value",
                                              runtime::Value(static_cast<double>(index + 1U))),
                  "retained snapshot fixture did not store its value");
        }
        saved = tree.description_snapshot();
        for (std::size_t index = 0U; index < queries.size(); ++index)
            check(observed(*saved, queries[index]) == static_cast<double>(index + 1U),
                  "retained query selected another source, scope or widget type");
        check(saved->find_key(queries[0].key) == nullptr &&
                  saved->find_key(queries[4].key) != nullptr &&
                  saved->find_key(queries[0].key, "/missing", "alpha", "Panel") == nullptr,
              "retained snapshot accepted an ambiguous or missing key");
        const auto* changed = tree.find_key("branch.0")->children().front().get();
        check(tree.set_retained_value(changed->identity(), "value", runtime::Value(77.0)),
              "retained snapshot fixture value did not change");
        check(observed(*tree.description_snapshot(), queries[0]) == 77.0 &&
                  observed(*saved, queries[0]) == 1.0,
              "retained write mutated a previously captured snapshot");
        static_cast<void>(tree.reconcile(root(true)));
        check(tree.description_snapshot()->find_key(
                  queries[0].key, queries[0].source, queries[0].scope, queries[0].type) == nullptr,
              "an empty duplicate stopped a retained query from being ambiguous");
    }
    check(observed(*saved, queries[0]) == 1.0 && observed(*saved, queries[4]) == 5.0,
          "retained snapshot lost its keys or values when its tree was destroyed");
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

void test_a_slot_in_a_widgets_generated_rows_takes_the_callers_content() {
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("invalidation-table-slots", bundle);
    // The row template declares the slot: it describes inside Listing, so Listing's callers fill
    // it, the slot being Listing's only one.
    activate(application, "invalidation-table-slots", R"(
component RowView(cells: any, key: key, selected: boolean) {
  Panel(key: key) {
    Text(key: format("{0}.label", key), text: cells.name)
    Slot(name: "content")
  }
}
component Listing() {
  Table(key: "slot.table", columns: [{ id: "name", header: "Name" }],
        rows: [{ key: "a", cells: { name: "A" } }, { key: "b", cells: { name: "B" } }],
        rowTemplate: RowView)
}
overlay Filled {
  state caption = "first";
  root Listing() {
    Text(key: "slot.caption", text: caption)
  }
}
)");
    ui::DescriptionBuilder builder(application);
    const ui::DescriptionBuildResult first = builder.build(runtime::LayerRole::overlay, "Filled");
    check(first.diagnostics.empty(), "the table slot fixture produced diagnostics");
    const std::shared_ptr<const ui::DescriptionNode> table = find(first.root, "slot.table");
    check(table != nullptr && table->children->size() == 2U, "the table did not provide its rows");
    check(builder.telemetry().described_rows == 0U,
          "projecting content into a component realized its table rows");
    for (std::size_t index = 0U; index < table->children->size(); ++index) {
        check(text(find(table->children->at(index), "slot.caption")) == "first",
              "a slot inside a table's row template did not take the caller's content");
    }
    const std::uint64_t described = builder.telemetry().described_rows;

    // New content reaches the rows already realized when they are next realized.
    write_state(application, "overlay Filled", "caption", runtime::Value("second"));
    const ui::DescriptionBuildResult second = builder.build(runtime::LayerRole::overlay, "Filled");
    const std::shared_ptr<const ui::DescriptionNode> reprovided = find(second.root, "slot.table");
    check(reprovided != nullptr && reprovided->children != table->children &&
              text(find(reprovided->children->at(0U), "slot.caption")) == "second" &&
              text(find(reprovided->children->at(1U), "slot.caption")) == "second" &&
              builder.telemetry().described_rows == described + 2U,
          "table rows kept the content their caller no longer passes");
}

/** Outgoing content has left the key index but is still in the tree. */
[[nodiscard]] const ui::RetainedNode* mounted(const ui::Surface& surface, const std::string_view key) {
    const auto find = [key](const auto& self, const ui::RetainedNode& node) -> const ui::RetainedNode* {
        if (node.description().key == key)
            return &node;
        for (const auto& child : node.children()) {
            if (const ui::RetainedNode* const found = self(self, *child); found != nullptr)
                return found;
        }
        return nullptr;
    };
    return surface.tree().root() != nullptr ? find(find, *surface.tree().root()) : nullptr;
}

/** A `contentHold` boundary shows what it has until what replaces it is whole, within a bound. */
void test_content_hold_keeps_outgoing_until_incoming_is_ready() {
    base::TaskPool pool(1U);
    base::TaskOwner owner(base::TaskExecution::asynchronous, {}, &pool);
    runtime::ApplicationContext application(
        "content-hold", runtime::ApplicationBundle::create(), {}, &owner);
    activate(application, "content-hold", R"(
animation Swap {
  from { opacity: 0 }
  to { opacity: 1 }
  duration: 100ms;
}
overlay Main {
  state page = "a";
  root Panel(key: "host", contentKey: page, contentTransition: Swap, contentHold: 200,
             contentTransitionMode: "TOGETHER",
             layout: { kind: "COLUMN", width: 200, height: 100 }) {
    if page == "a" {
      Panel(key: "a", layout: { width: 200, height: 100 })
    } else if page == "b" {
      Panel(key: "b", layout: { width: 200, height: 100 })
    } else {
      Panel(key: "c", layout: { width: 200, height: 100 })
    }
  }
}
)");
    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 200;
    environment.framebuffer_height = 100;
    environment.logical_width = 200.0;
    environment.logical_height = 100.0;
    ui::Surface surface("content-hold", application, runtime::LayerRole::overlay, "Main",
                        environment, {}, ui::WidgetRegistry{});
    std::int64_t now = 0;
    const auto frame = [&surface, &now](const std::int64_t advance_ms, const bool packet = true) {
        now += advance_ms * 1'000'000;
        static_cast<void>(surface.frame(now));
        if (packet)
            surface.note_packet_encoded();
    };
    const auto hold_of = [&surface](const std::string_view key) {
        const ui::RetainedNode* const node = mounted(surface, key);
        check(node != nullptr, "held content is not mounted: " + std::string(key));
        return node->content_hold();
    };
    const auto settle = [&frame] {
        for (int pass = 0; pass < 40; ++pass)
            frame(16);
    };
    settle();
    const std::string scope("overlay Main");

    // Nothing has reached a packet yet: the old page stays and the new one is offstage, to the bound.
    write_state(application, scope, "page", runtime::Value("b"));
    frame(0, false);
    frame(1, false);
    check(hold_of("a") == ui::ContentHold::outgoing && hold_of("b") == ui::ContentHold::incoming,
          "a changed key did not hold its outgoing content");
    check(mounted(surface, "b")->offstage_root(), "held content has no offstage root");
    frame(100, false);
    check(hold_of("b") == ui::ContentHold::incoming, "a hold ended before its deadline");
    frame(100, false);
    check(hold_of("b") == ui::ContentHold::none, "a hold outlived its deadline");
    settle();
    check(mounted(surface, "a") == nullptr, "outgoing content outlived its transition");

    // A packet that found nothing pending lets go on the next frame.
    write_state(application, scope, "page", runtime::Value("a"));
    frame(0, false);
    frame(1, false);
    check(hold_of("a") == ui::ContentHold::incoming, "the second change did not hold");
    frame(1);
    frame(1);
    check(hold_of("a") == ui::ContentHold::none, "a ready page was still held");
    settle();

    // Pending preparation under the incoming content keeps holding, past any packet.
    std::promise<void> finish;
    auto prepared = base::Pending<int>::start(
        owner, base::TaskKind::image, base::TaskPriority::frame_critical, "incoming",
        [done = finish.get_future().share()](const base::TaskCancellation&) {
            done.wait();
            return 1;
        });
    write_state(application, scope, "page", runtime::Value("b"));
    frame(0, false);
    frame(1, false);
    mounted(surface, "b")->observe_preparation(prepared.readiness());
    frame(1);
    frame(1);
    check(hold_of("b") == ui::ContentHold::incoming, "a hold ended with preparation pending");
    finish.set_value();
    check(owner.wait_for_completion(std::chrono::seconds(10)), "preparation did not complete");
    frame(1);
    frame(1);
    check(hold_of("b") == ui::ContentHold::none, "a hold outlived its preparation");
    settle();

    // Replaced again while held: what never showed goes at once, and what shows is kept.
    write_state(application, scope, "page", runtime::Value("c"));
    frame(0, false);
    frame(1, false);
    write_state(application, scope, "page", runtime::Value("a"));
    frame(1, false);
    frame(1, false);
    check(mounted(surface, "c") == nullptr, "content never shown was kept for an exit");
    check(hold_of("b") == ui::ContentHold::outgoing && hold_of("a") == ui::ContentHold::incoming,
          "a second change lost what was showing");

    // Back to what is showing: nothing was on its way.
    write_state(application, scope, "page", runtime::Value("b"));
    frame(1, false);
    frame(1, false);
    check(hold_of("b") == ui::ContentHold::none &&
              mounted(surface, "b")->lifecycle() == ui::RetainedLifecycle::attached,
          "returning to the page on screen did not end its hold");
}

/** Held content is on screen for nobody, so a wheel cannot reach it or scroll what it covers. */
void test_held_scrollers_take_no_wheel() {
    base::TaskPool pool(1U);
    base::TaskOwner owner(base::TaskExecution::asynchronous, {}, &pool);
    runtime::ApplicationContext application(
        "held-scrollers", runtime::ApplicationBundle::create(), {}, &owner);
    activate(application, "held-scrollers", R"(
animation Swap {
  from { opacity: 0 }
  to { opacity: 1 }
  duration: 100ms;
}
overlay Main {
  state page = "a";
  root Panel(key: "host", contentKey: page, contentTransition: Swap, contentHold: 200,
             contentTransitionMode: "TOGETHER",
             layout: { kind: "COLUMN", width: 200, height: 100 }) {
    if page == "a" {
      Scroll(key: "scroll.a", vertical: true, layout: { width: 200, height: 100 }) {
        Panel(layout: { width: 200, height: 400 })
      }
    } else {
      Scroll(key: "scroll.b", vertical: true, layout: { width: 200, height: 100 }) {
        Panel(layout: { width: 200, height: 400 })
      }
    }
  }
}
)");
    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 200;
    environment.framebuffer_height = 100;
    environment.logical_width = 200.0;
    environment.logical_height = 100.0;
    ui::Surface surface("held-scrollers", application, runtime::LayerRole::overlay, "Main",
                        environment, {}, ui::WidgetRegistry{});
    std::int64_t now = 0;
    for (int pass = 0; pass < 4; ++pass) {
        static_cast<void>(surface.frame(now += 16'000'000));
        surface.note_packet_encoded();
    }
    const auto listed = [&surface](const std::string_view key) {
        const ui::RetainedNode* const node = mounted(surface, key);
        return node != nullptr &&
               std::ranges::any_of(surface.render_scrollers(), [node](const auto& scroller) {
                   return scroller.identity == node->identity();
               });
    };
    check(listed("scroll.a"), "a scroller on screen was not offered to the render thread");
    write_state(application, "overlay Main", "page", runtime::Value("b"));
    static_cast<void>(surface.frame(now += 1'000'000));
    static_cast<void>(surface.frame(now += 1'000'000));
    check(mounted(surface, "scroll.b") != nullptr && !listed("scroll.a") && !listed("scroll.b"),
          "held or outgoing scrollers were offered to the render thread");
}

/** A widget with an input hook that ignores the wheel must not keep it from scrolling the page. */
void test_only_wheel_consumers_are_listed() {
    const ui::WidgetRegistry widgets;
    const auto types = widgets.wheel_consumer_types();
    check(std::ranges::contains(types, std::string_view("ZoomView")),
          "a widget that zooms under the wheel was not listed as taking it");
    check(widgets.find("RangeSlider") != nullptr && widgets.find("RangeSlider")->input.event &&
              !std::ranges::contains(types, std::string_view("RangeSlider")),
          "a widget with an input hook that ignores the wheel was listed as taking it");
}

/** Inline preparation has nothing to wait for, so deterministic frames never hold. */
void test_content_hold_needs_asynchronous_preparation() {
    runtime::ApplicationContext application("content-hold", runtime::ApplicationBundle::create());
    activate(application, "content-hold", R"(
animation Swap {
  from { opacity: 0 }
  to { opacity: 1 }
  duration: 100ms;
}
overlay Main {
  state page = "a";
  root Panel(key: "host", contentKey: page, contentTransition: Swap, contentHold: 200,
             layout: { kind: "COLUMN", width: 200, height: 100 }) {
    if page == "a" {
      Panel(key: "a", layout: { width: 200, height: 100 })
    } else {
      Panel(key: "b", layout: { width: 200, height: 100 })
    }
  }
}
)");
    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 200;
    environment.framebuffer_height = 100;
    environment.logical_width = 200.0;
    environment.logical_height = 100.0;
    ui::Surface surface("content-hold", application, runtime::LayerRole::overlay, "Main",
                        environment, {}, ui::WidgetRegistry{});
    static_cast<void>(surface.frame(0));
    write_state(application, "overlay Main", "page", runtime::Value("b"));
    static_cast<void>(surface.frame(16'000'000));
    static_cast<void>(surface.frame(32'000'000));
    const ui::RetainedNode* const incoming = mounted(surface, "b");
    check(incoming != nullptr, "the changed key did not mount its content");
    check(incoming->content_hold() == ui::ContentHold::none,
          "deterministic preparation held content");
}

void test_readiness_reaches_cached_widgets_and_each_surface() {
    for (const bool fail : {false, true}) {
        base::TaskPool pool(1U);
        base::TaskOwner owner(base::TaskExecution::asynchronous, {}, &pool);
        // A broken promise also releases the worker if a frame assertion throws.
        std::promise<void> release;
        auto resource = base::Pending<std::string>::start(
            owner, base::TaskKind::image, base::TaskPriority::frame_critical, "caption",
            [ready = release.get_future().share(), fail](const base::TaskCancellation&) {
                ready.wait();
                if (fail)
                    throw std::runtime_error("caption preparation failed");
                return std::string("prepared");
            });
        runtime::ApplicationContext application(
            "readiness", runtime::ApplicationBundle::create(), {}, &owner);
        activate(application, "readiness", R"(
component Reader() {
  state count = 0;
  Panel(key: "reader") {
    Text(key: "dependent", text: "pending")
    Text(key: "counter", text: format("{0}", count))
  }
}
component Fixed() { Text(key: "fixed", text: "fixed") }
overlay Main {
  root Panel(key: "root") {
    Reader()
    Fixed()
  }
}
)");
        ui::WidgetRegistry widgets;
        ui::WidgetDescribePhase phase = widgets.find("Text")->describe;
        const ui::WidgetDescriptionHook original = phase.expand;
        phase.expand = [original, resource](ui::WidgetDescriptionScope& scope) {
            if (original != nullptr)
                original(scope);
            if (scope.description().key != "dependent")
                return;
            const base::PendingStatus status = scope.observe(resource.readiness());
            const std::string value = status == base::PendingStatus::ready ? *resource.get()
                                      : status == base::PendingStatus::failed ? "unavailable"
                                                                            : "pending";
            scope.description().properties.insert_or_assign(
                "text", runtime::ExpressionValue(runtime::Value(value)));
        };
        widgets.register_describe_phase("Text", std::move(phase));
        ui::DescriptionBuilder builder(application, widgets);
        const auto first = builder.build(runtime::LayerRole::overlay, "Main");
        const auto dependent = find(first.root, "dependent");
        check(text(dependent) == "pending", "pending description exposed a worker's payload");
        write_state(application, dependent->state_scope, "count", runtime::Value(1.0));
        const auto counted = builder.build(runtime::LayerRole::overlay, "Main");
        check(text(find(counted.root, "counter")) == "1" &&
                  find(counted.root, "dependent") == dependent,
              "an unrelated state write did not reuse the pending widget call");

        ui::SurfaceEnvironment environment;
        environment.framebuffer_width = 320;
        environment.framebuffer_height = 180;
        environment.logical_width = 320.0;
        environment.logical_height = 180.0;
        environment.reduced_motion = true;
        ui::Surface left("readiness.left", application, runtime::LayerRole::overlay, "Main",
                         environment, {}, widgets);
        ui::Surface right("readiness.right", application, runtime::LayerRole::overlay, "Main",
                          environment, {}, widgets);
        static_cast<void>(left.frame(0));
        static_cast<void>(right.frame(0));
        const auto right_pending = right.tree().find_key("dependent")->shared_description();
        release.set_value();
        check(owner.wait_for_completion(std::chrono::seconds(5)) && resource.get() == nullptr &&
                  !builder.readiness_changed(),
              "worker completion published readiness without owner adoption");

        static_cast<void>(left.frame(1));
        const std::string expected = fail ? "unavailable" : "prepared";
        check(text(left.tree().find_key("dependent")->shared_description()) == expected &&
                  text(right.tree().find_key("dependent")->shared_description()) == "pending" &&
                  builder.readiness_changed(),
              "one Surface consumed another observer's resource transition");
        const auto ready = builder.build(runtime::LayerRole::overlay, "Main");
        check(text(find(ready.root, "dependent")) == expected &&
                  ready.described_instances == 1U &&
                  find(ready.root, "counter") == find(counted.root, "counter") &&
                  find(ready.root, "fixed") == find(first.root, "fixed") &&
                  !builder.readiness_changed(),
              "resource adoption reused stale widget output or rebuilt unrelated descriptions");
        static_cast<void>(right.frame(1));
        check(text(right.tree().find_key("dependent")->shared_description()) == expected &&
                  right_pending->readiness.front().observed == base::PendingStatus::pending,
              "the second Surface missed readiness or mutated its prior description");
        const data::JsonValue inspected = ui::inspect_node(right, "dependent", 0U);
        const data::JsonValue* reads = inspected.find("readiness");
        check(reads != nullptr && reads->array() != nullptr && reads->array()->size() == 1U,
              "inspection omitted the node's resource dependency");
        const auto& read = reads->array()->front();
        check(read.find("label") != nullptr && read.find("status") != nullptr &&
                  *read.find("label")->string() == "caption" &&
                  *read.find("status")->string() == (fail ? "FAILED" : "READY"),
              "inspection did not expose the adopted resource identity and terminal state");
        const auto adoptions = [](const ui::Surface& surface) {
            for (const auto& counter : surface.profiler().snapshot().counters) {
                if (counter.counter == runtime::ProfilerCounter::tasks_adopted)
                    return counter.value;
            }
            throw std::runtime_error("task adoption telemetry was not published");
        };
        check(adoptions(left) == 1U && adoptions(right) == 1U,
              "one Surface profiler consumed the shared runtime's task accounting");
        static_cast<void>(left.frame(2));
        static_cast<void>(right.frame(2));
        check(adoptions(left) == 0U && adoptions(right) == 0U,
              "settled frames recounted a previous resource adoption");
    }
}

void test_preparation_reaches_cached_paint_without_redescribing() {
    base::TaskOwner owner(base::TaskExecution::asynchronous);
    std::promise<void> release;
    const auto resource = base::Pending<double>::start(
        owner, base::TaskKind::image, base::TaskPriority::frame_critical, "paint-width",
        [ready = release.get_future().share()](const base::TaskCancellation&) {
            ready.wait();
            return 72.0;
        });
    runtime::ApplicationContext application(
        "paint-readiness", runtime::ApplicationBundle::create(), {}, &owner);
    activate(application, "paint-readiness", R"(
overlay Main {
  root Panel(key: "root") {
    Panel(key: "dependent", style: { width: 100, height: 40 })
    Panel(key: "fixed", style: { width: 100, height: 40 })
  }
}
)");
    std::size_t dependent_paints = 0U;
    std::size_t fixed_paints = 0U;
    ui::WidgetRegistry widgets;
    ui::WidgetPresentPhase phase = widgets.find("Panel")->present;
    const auto original = phase.content;
    phase.content = [original, resource, &dependent_paints, &fixed_paints](
                        ui::WidgetRenderScope& scope) {
        if (original != nullptr)
            original(scope);
        if (scope.node().description().key == "fixed")
            ++fixed_paints;
        if (scope.node().description().key != "dependent")
            return;
        ++dependent_paints;
        const auto status = scope.observe(resource.readiness());
        const auto& bounds = scope.layout().bounds;
        scope.solid_rect(ui::Rect{bounds.x, bounds.y,
                                  status == base::PendingStatus::ready ? *resource.get() : 12.0,
                                  7.0},
                         ui::RenderColor{19U, 31U, 47U, 255U});
    };
    widgets.register_present_phase("Panel", std::move(phase));
    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 320;
    environment.framebuffer_height = 180;
    environment.logical_width = 320.0;
    environment.logical_height = 180.0;
    environment.reduced_motion = true;
    ui::Surface left("paint.left", application, runtime::LayerRole::overlay, "Main",
                     environment, {}, widgets);
    ui::Surface right("paint.right", application, runtime::LayerRole::overlay, "Main",
                      environment, {}, widgets);
    static_cast<void>(left.frame(0));
    static_cast<void>(right.frame(0));
    const auto left_description = left.tree().find_key("dependent")->shared_description();
    const auto right_description = right.tree().find_key("dependent")->shared_description();
    const std::size_t fixed_before = fixed_paints;
    const std::size_t painted_before = dependent_paints;
    const auto painted_width = [](const ui::Surface& surface) {
        for (const auto& command : surface.render_commands().commands()) {
            if (const auto* rect = std::get_if<ui::SolidRectRenderCommand>(&command);
                rect != nullptr && rect->bounds.height == 7.0)
                return rect->bounds.width;
        }
        throw std::runtime_error("prepared paint disappeared from its retained command stream");
    };
    check(painted_width(left) == 12.0 && painted_width(right) == 12.0,
          "pending paint exposed unpublished resource data");
    static_cast<void>(left.frame(0));
    static_cast<void>(right.frame(0));
    check(dependent_paints == painted_before,
          "pending paint prevented retained fragment reuse");
    release.set_value();
    check(owner.wait_for_completion(std::chrono::seconds(5)),
          "paint preparation did not complete");
    static_cast<void>(left.frame(1));
    static_cast<void>(right.frame(1));
    check(painted_width(left) == 72.0 && painted_width(right) == 72.0 &&
              fixed_paints == fixed_before &&
              left.tree().find_key("dependent")->shared_description() == left_description &&
              right.tree().find_key("dependent")->shared_description() == right_description,
          "paint adoption missed a Surface, rebuilt unrelated paint, or redescribed its consumer");
    check(dependent_paints == painted_before + 2U,
          "cached paint did not independently observe the shared completion");
    const auto inspected = ui::inspect_node(right, "dependent", 0U);
    const auto* reads = inspected.find("readiness");
    check(reads != nullptr && reads->array() != nullptr && reads->array()->size() == 1U &&
              *reads->array()->front().find("phase")->string() == "paint",
          "paint readiness was not exposed separately from packet preparation");
}

void test_runtime_image_metadata_reaches_cached_fit() {
    base::TaskOwner owner(base::TaskExecution::deterministic);
    resource::RuntimeImageStore images;
    images.publish("fixed", {1U, 1U}, resource::TextureSampling::linear, {0U, 0U, 0U, 255U});
    auto registry = std::make_shared<const resource::SvgImageRegistry>(
        owner, std::vector<resource::ImageSource>{}, &ui::validate_svg_image_geometry, &images, nullptr);
    runtime::ApplicationContext application(
        "image-fit-readiness", runtime::ApplicationBundle::create(), {}, &owner);
    activate(application, "image-fit-readiness", R"(
overlay Main {
  root Panel(layout: { kind: "ROW", width: 200, height: 100 }) {
    Image(key: "photo", image: "photo", fit: "CONTAIN", layout: { width: 100, height: 100 })
    Image(key: "fixed", image: "fixed", fit: "CONTAIN", layout: { width: 100, height: 100 })
  }
}
)");
    std::size_t photo_paints = 0U;
    std::size_t fixed_paints = 0U;
    ui::WidgetRegistry widgets;
    auto phase = widgets.find("Image")->present;
    const auto original = phase.content;
    phase.content = [original, &photo_paints, &fixed_paints](ui::WidgetRenderScope& scope) {
        if (scope.node().description().key == "photo")
            ++photo_paints;
        else
            ++fixed_paints;
        original(scope);
    };
    widgets.register_present_phase("Image", std::move(phase));
    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 200;
    environment.framebuffer_height = 100;
    environment.logical_width = 200.0;
    environment.logical_height = 100.0;
    environment.reduced_motion = true;
    ui::Surface left("fit.left", application, runtime::LayerRole::overlay, "Main",
                     environment, {}, widgets, {}, nullptr, {}, {}, registry);
    ui::Surface right("fit.right", application, runtime::LayerRole::overlay, "Main",
                      environment, {}, widgets, {}, nullptr, {}, {}, registry);
    for (int pass = 0; pass < 3; ++pass) {
        static_cast<void>(left.frame(0));
        static_cast<void>(right.frame(0));
    }
    const auto description = left.tree().find_key("photo")->shared_description();
    const auto fixed_before = fixed_paints;
    const auto fit = [](const ui::Surface& surface) {
        for (const auto& command : surface.render_commands().commands()) {
            if (const auto* image = std::get_if<ui::ImageRenderCommand>(&command);
                image != nullptr && image->texture == "photo")
                return ui::Size{image->bounds.width, image->bounds.height};
        }
        throw std::runtime_error("runtime image lost its retained symbolic draw");
    };
    const auto frame = [&](const std::int64_t now, const ui::Size expected) {
        const auto a = left.frame(now);
        const auto b = right.frame(now);
        check(fit(left) == expected && fit(right) == expected &&
                  a.operations.layout_measured_nodes == 0U && b.operations.layout_measured_nodes == 0U &&
                  fixed_paints == fixed_before &&
                  left.tree().find_key("photo")->shared_description() == description,
              "image metadata missed a cached fit reader or invalidated unrelated layout/paint");
    };
    images.publish("photo", {2U, 1U}, resource::TextureSampling::linear,
                   std::vector<std::uint8_t>(8U, 255U));
    frame(1, {100.0, 50.0});
    const auto painted = photo_paints;
    images.publish("photo", {2U, 1U}, resource::TextureSampling::linear,
                   std::vector<std::uint8_t>(8U, 0U));
    frame(2, {100.0, 50.0});
    check(photo_paints == painted, "pixel-only runtime publication rebuilt image geometry");
    images.publish("photo", {1U, 2U}, resource::TextureSampling::linear,
                   std::vector<std::uint8_t>(8U, 255U));
    frame(3, {50.0, 100.0});
    images.release("photo");
    frame(4, {100.0, 100.0});
    images.publish("photo", {2U, 1U}, resource::TextureSampling::linear,
                   std::vector<std::uint8_t>(8U, 255U));
    frame(5, {100.0, 50.0});
}

void test_preparation_restarts_cached_frame_hook() {
    base::TaskOwner owner(base::TaskExecution::asynchronous);
    std::promise<void> release;
    const auto resource = base::Pending<double>::start(
        owner, base::TaskKind::image, base::TaskPriority::frame_critical, "frame-width",
        [ready = release.get_future().share()](const base::TaskCancellation&) {
            ready.wait();
            return 72.0;
        });
    runtime::ApplicationContext application(
        "frame-readiness", runtime::ApplicationBundle::create(), {}, &owner);
    activate(application, "frame-readiness", R"(
overlay Main {
  root Panel {
    Panel(key: "dependent", layout: { width: 100, height: 40 })
    Panel(key: "fixed", layout: { width: 100, height: 40 })
  }
}
)");
    std::size_t callbacks = 0U;
    std::size_t fixed_callbacks = 0U;
    ui::WidgetRegistry widgets;
    widgets.register_frame_phase("Panel", ui::WidgetFramePhase{
        [resource, &callbacks, &fixed_callbacks](ui::WidgetInputScope& scope,
                                                const ui::WidgetFrameInfo& frame) {
            if (scope.node().description().key == "fixed")
                ++fixed_callbacks;
            if (scope.node().description().key != "dependent")
                return;
            ++callbacks;
            scope.node().observe_preparation(resource.readiness());
            if (const auto* width = resource.get(); width != nullptr) {
                check(frame.delta_nanos == 0, "waiting time leaked into a resumed frame hook");
                scope.set_retained("prepared.width", runtime::Value(*width), ui::DirtyReason::paint);
            }
        }, true});
    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 200;
    environment.framebuffer_height = 100;
    environment.logical_width = 200.0;
    environment.logical_height = 100.0;
    ui::Surface surface("frame.readiness", application, runtime::LayerRole::overlay, "Main",
                        environment, {}, widgets);
    for (int pass = 0; pass < 3; ++pass)
        static_cast<void>(surface.frame(0));
    const auto fixed_before = fixed_callbacks;
    const auto description = surface.tree().find_key("dependent")->shared_description();
    check(callbacks == 1U, "an unresolved frame resource either polled or never ran its first hook");
    release.set_value();
    check(owner.wait_for_completion(std::chrono::seconds(5)), "frame resource did not finish");
    static_cast<void>(surface.frame(0));
    const auto* width = surface.tree().find_key("dependent")->retained_value("prepared.width");
    check(callbacks == 2U && fixed_callbacks == fixed_before &&
              width != nullptr && width->number() != nullptr && *width->number() == 72.0 &&
              surface.tree().find_key("dependent")->shared_description() == description,
          "resource adoption did not restart only its frame consumer at the same owner timestamp");
    static_cast<void>(surface.frame(0));
    check(callbacks == 2U, "a terminal frame resource retained a perpetual callback");
}

void test_fresh_callbacks_keep_their_own_cells(const bool through_function) {
    runtime::ApplicationContext application("callback-cells",
                                             runtime::ApplicationBundle::create());
    const std::string factory = through_function
        ? "fn makeCounter() { var n = 0; fn step() { n += 1; return n; } "
          "return () => step(); }\n"
        : "fn makeCounter() { var n = 0; return () => { n += 1; return n; }; }\n";
    activate(application, "callback-cells", factory + R"(
overlay Main {
  state tick = 0;
  derived counter = makeCounter();
  root Panel {
    Text(key: "tick", text: format("{0}", tick))
    Button(key: "counter", label: "Count", onClick: () => counter())
  }
}
)");
    ui::DescriptionBuilder builder(application);
    const auto first = builder.build(runtime::LayerRole::overlay, "Main");
    const auto previous = find(first.root, "counter");
    check(first.diagnostics.empty() && previous != nullptr,
          "callback cell fixture did not describe its first button");
    check(previous->properties.at("onClick").action() != nullptr,
          "callback cell fixture retained a function-valued property, not an action");
    write_state(application, previous->state_scope, "tick", runtime::Value(1.0));
    const auto second = builder.build(runtime::LayerRole::overlay, "Main");
    check(first.diagnostics.empty() && second.diagnostics.empty(),
          "callback cell fixture could not describe a fresh factory");
    const auto old_handler = *previous->properties.at("onClick").action();
    const auto new_handler = *find(second.root, "counter")->properties.at("onClick").action();
    runtime::ExpressionRuntime expressions(application.host(),
                                            application.bundle()->action_registry());
    NoExternalScriptEffects effects;
    const auto old_result = expressions.run_script(old_handler->script, {}, effects);
    const auto new_result = expressions.run_script(new_handler->script, {}, effects);
    check(old_result.kind == runtime::ScriptOutcome::Kind::finished &&
              new_result.kind == runtime::ScriptOutcome::Kind::finished &&
              old_result.value == runtime::Value(1.0) && new_result.value == runtime::Value(1.0),
          "description reuse aliased fresh callbacks with equal-valued mutable captures");
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
        test_retained_snapshots_disambiguate_keys_and_keep_prior_values();
        test_a_retained_write_beside_a_repeater_describes_only_its_reader();
        test_a_slot_in_lazy_rows_takes_the_callers_content();
        test_a_slot_in_a_widgets_generated_rows_takes_the_callers_content();
        test_content_hold_keeps_outgoing_until_incoming_is_ready();
        test_content_hold_needs_asynchronous_preparation();
        test_held_scrollers_take_no_wheel();
        test_only_wheel_consumers_are_listed();
        test_readiness_reaches_cached_widgets_and_each_surface();
        test_preparation_reaches_cached_paint_without_redescribing();
        test_runtime_image_metadata_reaches_cached_fit();
        test_preparation_restarts_cached_frame_hook();
        test_fresh_callbacks_keep_their_own_cells(false);
        test_fresh_callbacks_keep_their_own_cells(true);
        test_a_failed_build_changes_nothing_outside_the_builder();
        std::cout << "strata_description_invalidation_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_description_invalidation_tests: " << error.what() << '\n';
        return 1;
    }
}
