#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "runtime/expression.hpp"

// StrataScript's statements. A run is an explicit stack of frames, one per function call a
// statement waits on, each a stack of cursors through the blocks it is inside, so a handler can
// stop at an `await` and resume when the answer arrives. Expressions are the evaluator's; a
// function called inside one runs to completion as a nested machine sharing the handler's
// transaction.
namespace strata::runtime {

/** Where a frame is in one block: the next statement, and the scope its locals extend. */
struct ScriptCursor final {
    enum class Kind : std::uint8_t { block, while_loop, for_loop, try_block, catch_block };

    Kind kind = Kind::block;
    BlockId block = no_program_id;
    std::uint32_t next = 0U;
    ExpressionScope scope;
    /** A loop's or try's own statement, and the scope it ran in. */
    const ProgramStatement* statement = nullptr;
    ExpressionScope outer;
    /** A for loop's items and the position of the next. */
    Value items;
    std::size_t index = 0U;
    /** The functions the block declares, closing over its scope as it grows. */
    std::shared_ptr<FunctionGroup> functions;
};

/** One function call in progress. */
struct ScriptFrame final {
    std::shared_ptr<const LambdaValue> function;
    std::vector<ScriptCursor> cursors;
    /** The statement waiting on the callee frame above, or on an action's answer. */
    const ProgramStatement* awaiting = nullptr;
};

struct ScriptRun final {
    std::vector<ScriptFrame> frames;
    ScriptTransaction transaction;
    std::uint64_t steps = 0U;
    ScriptEffects* effects = nullptr;
    ExpressionValue result;
};

namespace {

/** Counts a nesting for as long as it lives. */
class Nesting final {
  public:
    explicit Nesting(std::size_t& depth) : depth_(depth) {
        ++depth_;
    }
    Nesting(const Nesting&) = delete;
    Nesting& operator=(const Nesting&) = delete;
    ~Nesting() {
        --depth_;
    }

  private:
    std::size_t& depth_;
};

[[nodiscard]] Value error_value(std::string code, std::string message) {
    return Value(std::vector<std::pair<std::string, Value>>{
        {"code", Value(std::move(code))},
        {"message", Value(std::move(message))},
    });
}

/** `left op right` for a compound assignment, as the binary operators compute it. */
[[nodiscard]] std::optional<Value> combine(const AssignmentOperator operation, const Value& left,
                                           const Value& right) {
    if (operation == AssignmentOperator::add &&
        (left.string() != nullptr || right.string() != nullptr)) {
        return Value(display_string(left) + display_string(right));
    }
    // A list grows by what is added: one item, or each item of a list.
    if (operation == AssignmentOperator::add && left.list() != nullptr) {
        std::vector<Value> items = left.list()->values;
        if (right.list() != nullptr)
            items.insert(items.end(), right.list()->values.begin(), right.list()->values.end());
        else
            items.push_back(right);
        return Value(std::move(items));
    }
    const auto numeric = [](const Value& value) -> std::optional<double> {
        if (value.number() != nullptr)
            return *value.number();
        if (value.duration() != nullptr)
            return static_cast<double>(value.duration()->nanoseconds);
        return std::nullopt;
    };
    const std::optional<double> a = numeric(left);
    const std::optional<double> b = numeric(right);
    if (!a.has_value() || !b.has_value())
        return std::nullopt;
    double result = 0.0;
    switch (operation) {
    case AssignmentOperator::add: result = *a + *b; break;
    case AssignmentOperator::subtract: result = *a - *b; break;
    case AssignmentOperator::multiply: result = *a * *b; break;
    case AssignmentOperator::divide: result = *b == 0.0 ? 0.0 : *a / *b; break;
    case AssignmentOperator::modulo: result = *b == 0.0 ? 0.0 : std::fmod(*a, *b); break;
    case AssignmentOperator::assign: return right;
    }
    if (!std::isfinite(result))
        result = 0.0;
    if (left.duration() != nullptr || right.duration() != nullptr)
        return Value(DurationValue{static_cast<std::int64_t>(result)});
    return Value(result);
}

[[nodiscard]] const Value& data_of(const ExpressionValue& value) {
    static const Value null;
    const Value* data = value.data_value();
    return data != nullptr ? *data : null;
}

} // namespace

/**
 * Runs frames until none is left, or until one waits on an answer. A machine for a handler runs
 * the handler's own frames; one for a function called inside an expression runs that call's,
 * writing to the handler's transaction when there is one.
 */
class ScriptMachine final {
  public:
    ScriptMachine(ExpressionRuntime& runtime, ScriptRun* handler, std::vector<ScriptFrame>& frames,
                  const bool may_wait, DependencyRecorder* reads = nullptr)
        : runtime_(runtime), handler_(handler), frames_(frames), may_wait_(may_wait),
          reads_(reads) {}

    /** Calls a function with a block body: a frame in the scope its parameters are bound in. */
    void enter(std::shared_ptr<const LambdaValue> function, ExpressionScope scope) {
        const BlockId block = function->declaration().block;
        frames_.push_back(ScriptFrame{std::move(function), {}, nullptr});
        push_block(frames_.size() - 1U, ScriptCursor::Kind::block, block, std::move(scope),
                   nullptr);
    }

    /** True once the run waits on an answer; false once every frame has returned. */
    [[nodiscard]] bool run() {
        while (!frames_.empty()) {
            try {
                step();
                if (waiting_)
                    return true;
            } catch (const ScriptThrow& thrown) {
                if (!unwind(thrown.value))
                    throw;
            }
        }
        return false;
    }

    /** Resumes the statement that waited with its answer. */
    void deliver(Value answer) {
        const std::size_t frame = frames_.size() - 1U;
        const ProgramStatement* statement = std::exchange(frames_[frame].awaiting, nullptr);
        if (statement != nullptr)
            complete(frame, *statement, ExpressionValue(std::move(answer)));
    }

    /** Resumes the statement that waited by throwing the error its request failed with. */
    void raise(Value error) {
        if (!unwind(error))
            throw ScriptThrow{std::move(error)};
    }

    [[nodiscard]] ExpressionValue result() const {
        return result_;
    }

  private:
    [[nodiscard]] const Program& program(const std::size_t frame) const {
        return *frames_[frame].function->program;
    }

    void count_step() {
        std::uint64_t& steps = handler_ != nullptr ? handler_->steps : steps_;
        if (++steps > script_step_budget) {
            // Not catchable: a runaway loop ends the handler.
            steps = 0U;
            frames_.clear();
            throw ScriptThrow{error_value(
                "STRATA.SCRIPT.BUDGET_EXHAUSTED",
                "The handler ran more than " + std::to_string(script_step_budget) +
                    " steps without finishing.")};
        }
    }

    [[nodiscard]] ExpressionValue evaluate(const std::size_t frame, const ExpressionId expression,
                                           const ExpressionScope& scope) {
        Nesting scripting(runtime_.scripting_);
        // What the statements read is what the call depends on (see ExpressionRuntime::invoke).
        if (reads_ != nullptr)
            return runtime_.evaluate_node(program(frame), expression, scope, *reads_);
        DependencyRecorder ignored;
        return runtime_.evaluate_node(program(frame), expression, scope, ignored);
    }

    void step() {
        count_step();
        const std::size_t frame = frames_.size() - 1U;
        if (frames_[frame].cursors.empty()) {
            finish_frame(ExpressionValue{});
            return;
        }
        ScriptCursor& cursor = frames_[frame].cursors.back();
        const std::span<const ProgramStatement> statements =
            program(frame).statements(program(frame).block(cursor.block));
        if (cursor.next >= statements.size()) {
            end_of_block(frame);
            return;
        }
        execute(frame, statements[cursor.next++]);
    }

    /** Enters a block; a block declaring functions binds them over its scope first. */
    void push_block(const std::size_t frame, const ScriptCursor::Kind kind, const BlockId block,
                    ExpressionScope scope, const ProgramStatement* statement) {
        ScriptCursor cursor;
        cursor.kind = kind;
        cursor.block = block;
        cursor.statement = statement;
        cursor.outer = scope;
        cursor.scope = std::move(scope);
        bind_functions(frame, cursor);
        frames_[frame].cursors.push_back(std::move(cursor));
    }

    void bind_functions(const std::size_t frame, ScriptCursor& cursor) {
        const Program& code = program(frame);
        std::shared_ptr<FunctionGroup> group;
        for (const ProgramStatement& statement : code.statements(code.block(cursor.block))) {
            if (statement.kind != StatementKind::function)
                continue;
            if (group == nullptr) {
                group = std::make_shared<FunctionGroup>();
                group->program = frames_[frame].function->program;
            }
            group->functions.emplace_back(statement.name, statement.function);
        }
        if (group == nullptr)
            return;
        cursor.scope.bind_functions(group);
        group->close_over(cursor.scope);
        cursor.functions = std::move(group);
    }

    void end_of_block(const std::size_t frame) {
        ScriptCursor& cursor = frames_[frame].cursors.back();
        switch (cursor.kind) {
        case ScriptCursor::Kind::block:
        case ScriptCursor::Kind::try_block:
        case ScriptCursor::Kind::catch_block:
            frames_[frame].cursors.pop_back();
            return;
        case ScriptCursor::Kind::while_loop:
            if (truthy(data_of(evaluate(frame, cursor.statement->expression, cursor.outer)))) {
                restart(frame, cursor, cursor.outer);
            } else {
                frames_[frame].cursors.pop_back();
            }
            return;
        case ScriptCursor::Kind::for_loop:
            if (!advance(frame, cursor))
                frames_[frame].cursors.pop_back();
            return;
        }
    }

    /** Runs a loop's body again, in a fresh scope over the loop's. */
    void restart(const std::size_t frame, ScriptCursor& cursor, ExpressionScope scope) {
        cursor.next = 0U;
        cursor.scope = std::move(scope);
        cursor.functions.reset();
        bind_functions(frame, cursor);
    }

    /** Moves a for loop to its next item its filter keeps; false when there is none. */
    [[nodiscard]] bool advance(const std::size_t frame, ScriptCursor& cursor) {
        const ProgramStatement& loop = *cursor.statement;
        const ValueList* items = cursor.items.list();
        while (items != nullptr && cursor.index < items->values.size()) {
            const std::size_t position = cursor.index++;
            ExpressionScope scope = cursor.outer;
            ScopeFrame bound;
            bound.bindings.push_back(ScopeBinding{loop.name, true, ScopeStateBinding::cleared,
                                                  ExpressionValue(items->values[position]),
                                                  nullptr});
            if (loop.has_index) {
                bound.bindings.push_back(ScopeBinding{
                    loop.index_name, true, ScopeStateBinding::cleared,
                    ExpressionValue(Value(static_cast<double>(position))), nullptr});
            }
            scope.push(std::move(bound));
            if (loop.filter != no_program_id &&
                !truthy(data_of(evaluate(frame, loop.filter, scope)))) {
                continue;
            }
            restart(frame, cursor, std::move(scope));
            return true;
        }
        return false;
    }

    void execute(const std::size_t frame, const ProgramStatement& statement) {
        const ExpressionScope scope = frames_[frame].cursors.back().scope;
        switch (statement.kind) {
        case StatementKind::let_binding:
        case StatementKind::assign:
        case StatementKind::effect:
            if (std::optional<ExpressionValue> value = value_of(frame, statement, scope))
                complete(frame, statement, std::move(*value));
            return;
        case StatementKind::return_value:
            if (statement.expression == no_program_id) {
                finish_frame(ExpressionValue{});
            } else if (std::optional<ExpressionValue> value = value_of(frame, statement, scope)) {
                complete(frame, statement, std::move(*value));
            }
            return;
        case StatementKind::throw_value:
            throw ScriptThrow{data_of(evaluate(frame, statement.expression, scope))};
        case StatementKind::conditional: {
            const bool condition = truthy(data_of(evaluate(frame, statement.expression, scope)));
            const BlockId selected = condition ? statement.block : statement.otherwise;
            if (selected != no_program_id)
                push_block(frame, ScriptCursor::Kind::block, selected, scope, nullptr);
            return;
        }
        case StatementKind::when: {
            const Value subject = data_of(evaluate(frame, statement.expression, scope));
            for (const ProgramWhenBranch& branch : program(frame).branches(statement)) {
                if (branch.match != no_program_id &&
                    data_of(evaluate(frame, branch.match, scope)) != subject) {
                    continue;
                }
                push_block(frame, ScriptCursor::Kind::block, branch.block, scope, nullptr);
                return;
            }
            return;
        }
        case StatementKind::loop: {
            const ExpressionValue collection = evaluate(frame, statement.expression, scope);
            Value items = data_of(collection);
            if (items.list() == nullptr && items.kind() != ValueKind::null_value) {
                runtime_.script_error("STRATA.SCRIPT.NOT_A_LIST",
                                      "A for loop needs a list, but found " +
                                          std::string(items.state_type_id()) + ".");
            }
            ScriptCursor cursor;
            cursor.kind = ScriptCursor::Kind::for_loop;
            cursor.block = statement.block;
            cursor.statement = &statement;
            cursor.outer = scope;
            cursor.items = std::move(items);
            if (advance(frame, cursor))
                frames_[frame].cursors.push_back(std::move(cursor));
            return;
        }
        case StatementKind::while_loop: {
            if (!truthy(data_of(evaluate(frame, statement.expression, scope))))
                return;
            ScriptCursor cursor;
            cursor.kind = ScriptCursor::Kind::while_loop;
            cursor.block = statement.block;
            cursor.statement = &statement;
            cursor.outer = scope;
            restart(frame, cursor, scope);
            frames_[frame].cursors.push_back(std::move(cursor));
            return;
        }
        case StatementKind::break_loop:
        case StatementKind::continue_loop: {
            std::vector<ScriptCursor>& cursors = frames_[frame].cursors;
            while (!cursors.empty()) {
                const ScriptCursor::Kind kind = cursors.back().kind;
                const bool loop = kind == ScriptCursor::Kind::while_loop ||
                                  kind == ScriptCursor::Kind::for_loop;
                if (loop && statement.kind == StatementKind::continue_loop) {
                    // The loop's end decides whether it runs again.
                    ScriptCursor& cursor = cursors.back();
                    cursor.next = program(frame).block(cursor.block).statements_count;
                    return;
                }
                cursors.pop_back();
                if (loop)
                    return;
            }
            return;
        }
        case StatementKind::try_catch:
            push_block(frame, ScriptCursor::Kind::try_block, statement.block, scope, &statement);
            return;
        case StatementKind::function:
            // Bound when its block was entered.
            return;
        case StatementKind::state:
        case StatementKind::derived:
        case StatementKind::node:
            runtime_.script_error("STRATA.SCRIPT.UNSUPPORTED_STATEMENT",
                                  "A script block cannot declare state or interface nodes.");
        }
    }

    /**
     * A statement's value. An `await` of a function with a block calls it in a new frame, and of
     * an action performs it; either way the statement completes when the answer arrives.
     */
    [[nodiscard]] std::optional<ExpressionValue> value_of(const std::size_t frame,
                                                          const ProgramStatement& statement,
                                                          const ExpressionScope& scope) {
        const Program& code = program(frame);
        const ProgramExpression& node = code.expression(statement.expression);
        if (node.kind != ExpressionKind::await_value)
            return evaluate(frame, statement.expression, scope);
        if (const ProgramExpression& operand = code.expression(node.first);
            operand.kind == ExpressionKind::call) {
            const ExpressionValue callee = evaluate(frame, operand.first, scope);
            if (callee.lambda() != nullptr && *callee.lambda() != nullptr &&
                (*callee.lambda())->declaration().block != no_program_id) {
                if (frames_.size() >= script_call_depth) {
                    runtime_.script_error("STRATA.SCRIPT.CALL_DEPTH",
                                          "Calls nested deeper than " +
                                              std::to_string(script_call_depth) + ".");
                }
                std::vector<ExpressionValue> positional;
                std::vector<std::pair<Symbol, ExpressionValue>> named;
                for (const ProgramArgument& argument : code.arguments(operand)) {
                    ExpressionValue value = evaluate(frame, argument.value, scope);
                    if (argument.named)
                        named.emplace_back(argument.name, std::move(value));
                    else
                        positional.push_back(std::move(value));
                }
                DependencyRecorder ignored;
                ExpressionScope called = runtime_.function_scope(
                    **callee.lambda(), std::move(positional), std::move(named), ignored);
                frames_[frame].awaiting = &statement;
                enter(*callee.lambda(), std::move(called));
                return std::nullopt;
            }
        }
        ExpressionValue value = evaluate(frame, node.first, scope);
        if (value.action() == nullptr || *value.action() == nullptr)
            return value;
        return await_action(frame, statement, *value.action());
    }

    [[nodiscard]] std::optional<ExpressionValue>
    await_action(const std::size_t frame, const ProgramStatement& statement,
                 const std::shared_ptr<const ActionValue>& action) {
        if (handler_ == nullptr || handler_->effects == nullptr) {
            runtime_.script_error("STRATA.SCRIPT.EFFECT_OUTSIDE_HANDLER",
                                  "Only a handler performs actions.");
        }
        // What the handler wrote so far is committed before the action runs.
        flush();
        ScriptAnswer answer = handler_->effects->perform(action);
        switch (answer.kind) {
        case ScriptAnswer::Kind::value:
            return ExpressionValue(std::move(answer.value));
        case ScriptAnswer::Kind::failed:
            runtime_.script_error("STRATA.SCRIPT.ACTION_FAILED", answer.message);
        case ScriptAnswer::Kind::pending:
            break;
        }
        if (!may_wait_) {
            runtime_.script_error("STRATA.SCRIPT.AWAIT_IN_CALL",
                                  "A function waits on an answer, so it must be awaited.");
        }
        frames_[frame].awaiting = &statement;
        waiting_ = true;
        return std::nullopt;
    }

    void flush() {
        if (handler_->transaction.empty())
            return;
        // What follows the await is the same undo step.
        ScriptTransaction next;
        next.undo_label = handler_->transaction.undo_label;
        next.undo_coalesce = handler_->transaction.undo_coalesce;
        if (std::optional<std::string> rejected =
                handler_->effects->commit(std::exchange(handler_->transaction, std::move(next)))) {
            runtime_.script_error("STRATA.SCRIPT.COMMIT_REJECTED", *rejected);
        }
    }

    /** Finishes a statement with its value. */
    void complete(const std::size_t frame, const ProgramStatement& statement,
                  ExpressionValue value) {
        switch (statement.kind) {
        case StatementKind::let_binding: {
            ScriptCursor& cursor = frames_[frame].cursors.back();
            if (statement.mutable_binding) {
                static_cast<void>(cursor.scope.declare_variable(statement.name, std::move(value)));
            } else {
                ScopeFrame bound;
                bound.bindings.push_back(ScopeBinding{statement.name, true,
                                                      ScopeStateBinding::cleared,
                                                      std::move(value), nullptr});
                cursor.scope.push(std::move(bound));
            }
            if (cursor.functions != nullptr)
                cursor.functions->close_over(cursor.scope);
            return;
        }
        case StatementKind::assign:
            assign(frame, statement, std::move(value));
            return;
        case StatementKind::effect:
            if (value.action() != nullptr && *value.action() != nullptr)
                runtime_.perform_action(*value.action());
            return;
        case StatementKind::return_value:
            finish_frame(std::move(value));
            return;
        default:
            return;
        }
    }

    void finish_frame(ExpressionValue value) {
        frames_.pop_back();
        if (frames_.empty()) {
            result_ = std::move(value);
            return;
        }
        const std::size_t caller = frames_.size() - 1U;
        if (const ProgramStatement* statement = std::exchange(frames_[caller].awaiting, nullptr))
            complete(caller, *statement, std::move(value));
    }

    /**
     * Assigns a state or a `var`, or a field or item of one: the value along the path is rebuilt
     * with the new part, and the whole assigned.
     */
    void assign(const std::size_t frame, const ProgramStatement& statement, ExpressionValue value) {
        const Program& code = program(frame);
        const ExpressionScope& scope = frames_[frame].cursors.back().scope;
        std::vector<const ProgramExpression*> path;
        const ProgramExpression* root = &code.expression(statement.target);
        while (root->kind == ExpressionKind::property || root->kind == ExpressionKind::index) {
            path.push_back(root);
            root = &code.expression(root->first);
        }
        std::ranges::reverse(path);
        if (root->kind != ExpressionKind::variable) {
            runtime_.script_error("STRATA.SCRIPT.ASSIGNMENT_TARGET",
                                  "Only a state or a var can be assigned.");
        }
        if (statement.assignment != AssignmentOperator::assign) {
            const Value current = data_of(evaluate(frame, statement.target, scope));
            std::optional<Value> combined =
                combine(statement.assignment, current, data_of(value));
            if (!combined.has_value()) {
                runtime_.script_error("STRATA.SCRIPT.TYPE_MISMATCH",
                                      "A compound assignment needs numbers, durations, text or "
                                      "a list to add to.");
            }
            value = ExpressionValue(std::move(*combined));
        }
        const std::shared_ptr<const LexicalStateBinding>* state = scope.script_state(root->name);
        const ScopeLookup local = scope.lookup(root->name);
        if (state == nullptr && (local.binding == nullptr || local.binding->cell == nullptr)) {
            runtime_.script_error("STRATA.SCRIPT.ASSIGNMENT_TARGET",
                                  "'" + std::string(root->name.name()) +
                                      "' is neither a state nor a var.");
        }
        if (!path.empty()) {
            Value base;
            if (state != nullptr) {
                const Value* current = runtime_.script_state_value(**state);
                base = current != nullptr  ? *current
                       : local.value != nullptr ? data_of(*local.value)
                                              : Value{};
            } else {
                base = data_of(*local.binding->cell);
            }
            value = ExpressionValue(rebuilt(frame, scope, base, path, 0U, data_of(value)));
        }
        if (state != nullptr) {
            if (handler_ == nullptr) {
                runtime_.script_error("STRATA.SCRIPT.EFFECT_OUTSIDE_HANDLER",
                                      "State '" + std::string(root->name.name()) +
                                          "' can be written only by a handler.");
            }
            handler_->transaction.write(*state, data_of(value));
            return;
        }
        *local.binding->cell = std::move(value);
    }

    /** `base` with the part `path` leads to replaced by `value`. */
    [[nodiscard]] Value rebuilt(const std::size_t frame, const ExpressionScope& scope,
                                const Value& base,
                                const std::vector<const ProgramExpression*>& path,
                                const std::size_t position, Value value) {
        if (position == path.size())
            return value;
        const ProgramExpression& step = *path[position];
        if (step.kind == ExpressionKind::property) {
            if (base.object() == nullptr && base.kind() != ValueKind::null_value) {
                runtime_.script_error("STRATA.SCRIPT.ASSIGNMENT_TARGET",
                                      "Field '" + step.text + "' belongs to a record, not " +
                                          std::string(base.state_type_id()) + ".");
            }
            std::vector<std::pair<std::string, Value>> fields =
                base.object() != nullptr ? base.object()->fields
                                         : std::vector<std::pair<std::string, Value>>{};
            const auto found =
                std::ranges::find(fields, step.text, &std::pair<std::string, Value>::first);
            const Value child = found != fields.end() ? found->second : Value{};
            Value updated = rebuilt(frame, scope, child, path, position + 1U, std::move(value));
            if (found != fields.end())
                found->second = std::move(updated);
            else
                fields.emplace_back(step.text, std::move(updated));
            return Value(std::move(fields));
        }
        const Value index = data_of(evaluate(frame, step.second, scope));
        if (base.list() != nullptr) {
            const std::optional<std::size_t> position_in_list = bounded_index(index);
            std::vector<Value> items = base.list()->values;
            if (!position_in_list.has_value() || *position_in_list > items.size()) {
                runtime_.script_error("STRATA.SCRIPT.INDEX_OUT_OF_RANGE",
                                      "Index " + display_string(index) + " is outside a list of " +
                                          std::to_string(items.size()) + ".");
            }
            if (*position_in_list == items.size()) {
                items.push_back(rebuilt(frame, scope, Value{}, path, position + 1U,
                                        std::move(value)));
            } else {
                items[*position_in_list] = rebuilt(frame, scope, items[*position_in_list], path,
                                                   position + 1U, std::move(value));
            }
            return Value(std::move(items));
        }
        if (base.object() != nullptr || base.kind() == ValueKind::null_value) {
            std::vector<std::pair<std::string, Value>> fields =
                base.object() != nullptr ? base.object()->fields
                                         : std::vector<std::pair<std::string, Value>>{};
            const std::string key =
                index.string() != nullptr ? *index.string() : display_string(index);
            const auto found = std::ranges::find(fields, key, &std::pair<std::string, Value>::first);
            const Value child = found != fields.end() ? found->second : Value{};
            Value updated = rebuilt(frame, scope, child, path, position + 1U, std::move(value));
            if (found != fields.end())
                found->second = std::move(updated);
            else
                fields.emplace_back(key, std::move(updated));
            return Value(std::move(fields));
        }
        runtime_.script_error("STRATA.SCRIPT.ASSIGNMENT_TARGET",
                              "Only a list or a record has items to assign.");
    }

    /** Unwinds to the innermost try; false when no frame of this machine has one. */
    [[nodiscard]] bool unwind(const Value& error) {
        waiting_ = false;
        while (!frames_.empty()) {
            const std::size_t frame = frames_.size() - 1U;
            frames_[frame].awaiting = nullptr;
            std::vector<ScriptCursor>& cursors = frames_[frame].cursors;
            while (!cursors.empty()) {
                if (cursors.back().kind == ScriptCursor::Kind::try_block) {
                    const ProgramStatement& attempt = *cursors.back().statement;
                    ExpressionScope scope = cursors.back().outer;
                    cursors.pop_back();
                    ScopeFrame caught;
                    caught.bindings.push_back(ScopeBinding{attempt.name, true,
                                                           ScopeStateBinding::cleared,
                                                           ExpressionValue(error), nullptr});
                    scope.push(std::move(caught));
                    push_block(frame, ScriptCursor::Kind::catch_block, attempt.otherwise,
                               std::move(scope), &attempt);
                    return true;
                }
                cursors.pop_back();
            }
            frames_.pop_back();
        }
        return false;
    }

    ExpressionRuntime& runtime_;
    ScriptRun* handler_;
    std::vector<ScriptFrame>& frames_;
    bool may_wait_;
    DependencyRecorder* reads_;
    bool waiting_ = false;
    std::uint64_t steps_ = 0U;
    ExpressionValue result_;
};

const Value* ExpressionRuntime::script_state_value(const LexicalStateBinding& binding) {
    if (active_run_ == nullptr)
        return nullptr;
    if (const Value* written = active_run_->transaction.written(binding.address))
        return written;
    return active_run_->effects != nullptr ? active_run_->effects->state(binding) : nullptr;
}

bool ExpressionRuntime::name_undo_step(std::string label, std::optional<std::string> coalesce) {
    if (active_run_ == nullptr)
        return false;
    active_run_->transaction.undo_label = std::move(label);
    if (coalesce.has_value())
        active_run_->transaction.undo_coalesce = std::move(coalesce);
    return true;
}

void ExpressionRuntime::perform_action(const std::shared_ptr<const ActionValue>& action) {
    if (active_run_ == nullptr) {
        script_error("STRATA.SCRIPT.EFFECT_OUTSIDE_HANDLER",
                     "Only a handler performs actions.");
    }
    active_run_->transaction.actions.push_back(action);
}

bool ExpressionRuntime::constant_function(const LambdaValue& function) {
    // A top-level function: a declared one (no lambda expression) closing over no block.
    if (function.expression != no_program_id || function.group != nullptr)
        return false;
    const ProgramFunction& declaration = function.declaration();
    if (declaration.parameters_count != 0U || declaration.asynchronous)
        return false;
    const ProgramNodeKey key{function.program->serial(), function.function};
    if (const auto known = constant_functions_.find(key); known != constant_functions_.end())
        return known->second;
    // It reads nothing a call could change when every name it reads, and every name each
    // function it reaches reads past its own parameters, is a top-level function.
    const Program& program = *function.program;
    std::vector<std::uint32_t> pending{function.function};
    std::set<std::uint32_t> seen{function.function};
    bool constant = true;
    while (constant && !pending.empty()) {
        const ProgramFunction& reached = program.function(pending.back());
        pending.pop_back();
        for (const Symbol name : program.free_variables(reached)) {
            const std::optional<std::uint32_t> index = program.function_index(name.name());
            if (!index.has_value()) {
                constant = false;
                break;
            }
            if (seen.insert(*index).second)
                pending.push_back(*index);
        }
    }
    constant_functions_.emplace(key, constant);
    return constant;
}

ExpressionValue ExpressionRuntime::invoke(const LambdaValue& function,
                                          std::vector<ExpressionValue> positional,
                                          std::vector<std::pair<Symbol, ExpressionValue>> named,
                                          DependencyRecorder& reads) {
    if (call_depth_ >= script_call_depth) {
        script_error("STRATA.SCRIPT.CALL_DEPTH",
                     "Calls nested deeper than " + std::to_string(script_call_depth) + ".");
    }
    const bool constant = positional.empty() && named.empty() && constant_function(function);
    if (constant) {
        const auto known =
            constant_values_.find(ProgramNodeKey{function.program->serial(), function.function});
        if (known != constant_values_.end())
            return known->second;
    }
    const ProgramFunction& declaration = function.declaration();
    // A function with statements, called outside a handler, gives the same value while what it
    // reads is unchanged: a rebuild reuses its last value instead of running it again.
    if (!constant && active_run_ == nullptr && declaration.block != no_program_id &&
        !declaration.asynchronous) {
        return memoized(function, std::move(positional), std::move(named), reads);
    }
    ExpressionValue value = invoke_once(function, std::move(positional), std::move(named), reads);
    if (constant) {
        constant_values_.insert_or_assign(
            ProgramNodeKey{function.program->serial(), function.function}, value);
    }
    return value;
}

ExpressionValue ExpressionRuntime::memoized(const LambdaValue& function,
                                            std::vector<ExpressionValue> positional,
                                            std::vector<std::pair<Symbol, ExpressionValue>> named,
                                            DependencyRecorder& reads) {
    const ExpressionScope& closure =
        function.group != nullptr ? function.group->scope : function.captured;
    const std::string& owner = closure.component_path();
    std::vector<MemoizedCall>& calls =
        memoized_calls_[ProgramNodeKey{function.program->serial(), function.function}];
    const auto same_arguments = [&](const MemoizedCall& call) {
        return call.owner == owner && call.positional.size() == positional.size() &&
               call.named.size() == named.size() &&
               std::ranges::equal(call.positional, positional, same_expression_value) &&
               std::ranges::equal(call.named, named, [](const auto& left, const auto& right) {
                   return left.first == right.first &&
                          same_expression_value(left.second, right.second);
               });
    };
    const auto current = [&](const MemoizedCall& call) {
        for (const auto& [read, seen] : call.lexical) {
            if (!lexical_read_current(closure, read, seen.value))
                return false;
        }
        for (const auto& [path, read] : call.host) {
            static_cast<void>(path);
            if (read_host_dependency(read.key, closure) != read)
                return false;
        }
        return true;
    };
    for (auto call = calls.begin(); call != calls.end(); ++call) {
        if (!same_arguments(*call) || !current(*call))
            continue;
        // What the call read, the expression calling it read too.
        for (const auto& [read, seen] : call->lexical)
            reads.lexical(read, seen.value);
        for (const auto& [path, read] : call->host) {
            static_cast<void>(path);
            reads.host(read);
        }
        return call->result;
    }
    // The function's own parameters and locals are not what it depends on.
    DependencyRecorder recorded(&reads, DependencyRecorder::Keep::first,
                                DependencyRecorder::Keep::first);
    for (const Symbol name : function.program->declared(function.declaration()))
        recorded.hide(name);
    MemoizedCall call{owner, positional, named, {}, {}, {}};
    call.result = invoke_once(function, std::move(positional), std::move(named), recorded);
    call.lexical = std::move(recorded.lexical_reads());
    call.host = std::move(recorded.host_reads());
    // A few calls per function and owner are kept, the newest last.
    constexpr std::size_t kept_calls = 64U;
    if (calls.size() >= kept_calls)
        calls.erase(calls.begin());
    calls.push_back(std::move(call));
    return calls.back().result;
}

ExpressionValue ExpressionRuntime::invoke_once(
    const LambdaValue& function, std::vector<ExpressionValue> positional,
    std::vector<std::pair<Symbol, ExpressionValue>> named, DependencyRecorder& reads) {
    Nesting depth(call_depth_);
    const ProgramFunction& declaration = function.declaration();
    // The call's parameters and locals are its own: reads of them are hidden from the caller's
    // recorder, and only from this call's, so a caller's own names stay visible.
    DependencyRecorder call_reads(&reads, DependencyRecorder::Keep::none,
                                  DependencyRecorder::Keep::none);
    for (const Symbol name : function.program->declared(declaration))
        call_reads.hide(name);
    ExpressionScope scope =
        function_scope(function, std::move(positional), std::move(named), call_reads);
    if (declaration.body != no_program_id)
        return evaluate_node(*function.program, declaration.body, scope, call_reads);
    if (declaration.block == no_program_id)
        return ExpressionValue{};
    std::vector<ScriptFrame> frames;
    ScriptMachine machine(*this, active_run_, frames, false, &call_reads);
    machine.enter(std::make_shared<const LambdaValue>(function), std::move(scope));
    static_cast<void>(machine.run());
    return machine.result();
}

namespace {

/** Makes a run the one scripts write to, for as long as it lives. */
class ActiveRun final {
  public:
    ActiveRun(ScriptRun*& active, ScriptRun* run) : active_(active), previous_(active) {
        active_ = run;
    }
    ActiveRun(const ActiveRun&) = delete;
    ActiveRun& operator=(const ActiveRun&) = delete;
    ~ActiveRun() {
        active_ = previous_;
    }

  private:
    ScriptRun*& active_;
    ScriptRun* previous_;
};

[[nodiscard]] ScriptOutcome failed(Value error) {
    ScriptOutcome outcome;
    outcome.kind = ScriptOutcome::Kind::failed;
    outcome.error = std::move(error);
    return outcome;
}

/** A run with no frame left commits what it wrote and performs what it queued. */
[[nodiscard]] ScriptOutcome finished(ScriptRun& run, ScriptEffects& effects) {
    if (!run.transaction.empty()) {
        if (std::optional<std::string> rejected =
                effects.commit(std::exchange(run.transaction, {}))) {
            return failed(error_value("STRATA.SCRIPT.COMMIT_REJECTED", std::move(*rejected)));
        }
    }
    ScriptOutcome outcome;
    outcome.value = data_of(run.result);
    return outcome;
}

} // namespace

ScriptOutcome ExpressionRuntime::run_script(const std::shared_ptr<const LambdaValue>& function,
                                            std::vector<Value> arguments,
                                            ScriptEffects& effects) {
    auto run = std::make_shared<ScriptRun>();
    run->effects = &effects;
    ActiveRun active(active_run_, run.get());
    try {
        std::vector<ExpressionValue> positional;
        positional.reserve(arguments.size());
        for (Value& argument : arguments)
            positional.emplace_back(std::move(argument));
        DependencyRecorder ignored;
        ExpressionScope scope = function_scope(*function, std::move(positional), {}, ignored);
        const ProgramFunction& declaration = function->declaration();
        if (declaration.block == no_program_id) {
            // An expression body: its value, performed when it is an action.
            Nesting scripting(scripting_);
            run->result = declaration.body != no_program_id
                              ? evaluate_node(*function->program, declaration.body, scope, ignored)
                              : ExpressionValue{};
            if (run->result.action() != nullptr && *run->result.action() != nullptr) {
                perform_action(*run->result.action());
                run->result = ExpressionValue{};
            }
            return finished(*run, effects);
        }
        ScriptMachine machine(*this, run.get(), run->frames, true);
        machine.enter(function, std::move(scope));
        if (machine.run()) {
            ScriptOutcome outcome;
            outcome.kind = ScriptOutcome::Kind::waiting;
            outcome.run = run;
            return outcome;
        }
        run->result = machine.result();
        return finished(*run, effects);
    } catch (const ScriptThrow& thrown) {
        return failed(thrown.value);
    }
}

ScriptOutcome ExpressionRuntime::resume_script(const std::shared_ptr<ScriptRun>& run, Value answer,
                                               const bool request_failed, ScriptEffects& effects) {
    run->effects = &effects;
    run->steps = 0U;
    ActiveRun active(active_run_, run.get());
    try {
        ScriptMachine machine(*this, run.get(), run->frames, true);
        if (request_failed)
            machine.raise(error_value("STRATA.SCRIPT.ACTION_FAILED", display_string(answer)));
        else
            machine.deliver(std::move(answer));
        if (machine.run()) {
            ScriptOutcome outcome;
            outcome.kind = ScriptOutcome::Kind::waiting;
            outcome.run = run;
            return outcome;
        }
        run->result = machine.result();
        return finished(*run, effects);
    } catch (const ScriptThrow& thrown) {
        return failed(thrown.value);
    }
}

} // namespace strata::runtime
