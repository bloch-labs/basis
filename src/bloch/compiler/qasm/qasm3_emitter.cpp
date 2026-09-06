// Copyright 2025-2026 Akshay Pal (https://bloch-labs.com)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "bloch/compiler/qasm/qasm3_emitter.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

#include "bloch/compiler/ast/ast.hpp"
#include "bloch/support/error/bloch_error.hpp"

namespace bloch::compiler {
namespace {

[[nodiscard]] const FunctionDeclaration* find_main(const Program& program) noexcept {
    for (const auto& function : program.functions) {
        if (function->name == "main") {
            return function.get();
        }
    }
    return nullptr;
}

[[nodiscard]] bool contains_control_flow(const Statement& statement) noexcept {
    if (dynamic_cast<const IfStatement*>(&statement) != nullptr ||
        dynamic_cast<const TernaryStatement*>(&statement) != nullptr ||
        dynamic_cast<const ForStatement*>(&statement) != nullptr ||
        dynamic_cast<const WhileStatement*>(&statement) != nullptr) {
        return true;
    }

    const auto* block = dynamic_cast<const BlockStatement*>(&statement);
    if (block == nullptr) {
        return false;
    }
    for (const auto& child : block->statements) {
        if (contains_control_flow(*child)) {
            return true;
        }
    }
    return false;
}

class Emitter {
   public:
    [[nodiscard]] std::string emit(const Program& program) {
        const FunctionDeclaration* main = find_main(program);
        if (main == nullptr || main->body == nullptr) {
            throw std::runtime_error("OpenQASM 3 emission requires a main function");
        }

        output_ = "OPENQASM 3.0;\ninclude \"stdgates.inc\";\n";
        emit_block(*main->body);
        return output_;
    }

   private:
    std::string output_;
    int indentation_ = 0;

    void append_line(std::string_view text) {
        output_.append(static_cast<std::size_t>(indentation_) * 4, ' ');
        output_.append(text).append("\n");
    }

    [[noreturn]] static void unsupported(const ASTNode& node, std::string_view construct) {
        throw support::BlochError(support::ErrorCategory::Generic, node.line, node.column,
                                  "OpenQASM 3 emission does not support " + std::string(construct));
    }

    [[nodiscard]] static std::string emit_type(const Type& type) {
        if (const auto* primitive = dynamic_cast<const PrimitiveType*>(&type)) {
            if (primitive->name == "qubit" || primitive->name == "bit") {
                return primitive->name;
            }
            if (primitive->name == "boolean") {
                return "bool";
            }
            if (primitive->name == "int") {
                return "int[32]";
            }
            if (primitive->name == "long") {
                return "int[64]";
            }
            if (primitive->name == "float") {
                return "float[64]";
            }
            unsupported(type, "type '" + primitive->name + "'");
        }

        if (const auto* array = dynamic_cast<const ArrayType*>(&type)) {
            const auto* element = dynamic_cast<const PrimitiveType*>(array->elementType.get());
            if (element == nullptr || (element->name != "qubit" && element->name != "bit") ||
                array->size <= 0) {
                unsupported(type, "this array type");
            }
            return element->name + "[" + std::to_string(array->size) + "]";
        }

        unsupported(type, "this type");
    }

    [[nodiscard]] static bool is_qubit_type(const Type& type) noexcept {
        if (const auto* primitive = dynamic_cast<const PrimitiveType*>(&type)) {
            return primitive->name == "qubit";
        }
        const auto* array = dynamic_cast<const ArrayType*>(&type);
        if (array == nullptr) {
            return false;
        }
        const auto* element = dynamic_cast<const PrimitiveType*>(array->elementType.get());
        return element != nullptr && element->name == "qubit";
    }

    [[nodiscard]] static std::string emit_literal(const LiteralExpression& literal) {
        std::string value = literal.value;
        if (literal.literalType == "bit" && value.ends_with('b')) {
            value.pop_back();
        } else if ((literal.literalType == "float" && value.ends_with('f')) ||
                   (literal.literalType == "long" && value.ends_with('L'))) {
            value.pop_back();
        }
        return value;
    }

    [[nodiscard]] std::string emit_expression(const Expression& expression) {
        if (const auto* literal = dynamic_cast<const LiteralExpression*>(&expression)) {
            return emit_literal(*literal);
        }
        if (const auto* variable = dynamic_cast<const VariableExpression*>(&expression)) {
            return variable->name;
        }
        if (const auto* binary = dynamic_cast<const BinaryExpression*>(&expression)) {
            return "(" + emit_expression(*binary->left) + " " + binary->op + " " +
                   emit_expression(*binary->right) + ")";
        }
        if (const auto* unary = dynamic_cast<const UnaryExpression*>(&expression)) {
            return unary->op + emit_expression(*unary->right);
        }
        if (const auto* parenthesized = dynamic_cast<const ParenthesizedExpression*>(&expression)) {
            return "(" + emit_expression(*parenthesized->expression) + ")";
        }
        if (const auto* index = dynamic_cast<const IndexExpression*>(&expression)) {
            return emit_expression(*index->collection) + "[" + emit_expression(*index->index) + "]";
        }
        if (const auto* measure = dynamic_cast<const MeasureExpression*>(&expression)) {
            return "measure " + emit_expression(*measure->qubit);
        }
        if (const auto* assignment = dynamic_cast<const AssignmentExpression*>(&expression)) {
            return assignment->name + " = " + emit_expression(*assignment->value);
        }
        if (const auto* postfix = dynamic_cast<const PostfixExpression*>(&expression)) {
            const std::string operation = postfix->op == "++" ? " += 1" : " -= 1";
            return emit_expression(*postfix->left) + operation;
        }
        if (const auto* array = dynamic_cast<const ArrayLiteralExpression*>(&expression)) {
            std::string result = "{";
            for (std::size_t index = 0; index < array->elements.size(); ++index) {
                if (index > 0) {
                    result.append(", ");
                }
                result.append(emit_expression(*array->elements[index]));
            }
            return result.append("}");
        }
        unsupported(expression, "this expression");
    }

    [[nodiscard]] std::string emit_condition(const Expression& expression) {
        std::string condition = emit_expression(expression);
        if (condition.starts_with('(') && condition.ends_with(')')) {
            return condition;
        }
        return "(" + condition + ")";
    }

    void emit_block(const BlockStatement& block) {
        for (const auto& statement : block.statements) {
            emit_statement(*statement);
        }
    }

    void emit_branch(const Statement& statement) {
        ++indentation_;
        if (const auto* block = dynamic_cast<const BlockStatement*>(&statement)) {
            emit_block(*block);
        } else {
            emit_statement(statement);
        }
        --indentation_;
    }

    void emit_call(const CallExpression& call) {
        const auto* callee = dynamic_cast<const VariableExpression*>(call.callee.get());
        if (callee == nullptr) {
            unsupported(call, "this call target");
        }

        const std::string& name = callee->name;
        if (name == "h" || name == "x" || name == "y" || name == "z") {
            if (call.arguments.size() != 1) {
                unsupported(call, "invalid gate arguments");
            }
            append_line(name + " " + emit_expression(*call.arguments[0]) + ";");
            return;
        }
        if (name == "cx") {
            if (call.arguments.size() != 2) {
                unsupported(call, "invalid cx arguments");
            }
            append_line("cx " + emit_expression(*call.arguments[0]) + ", " +
                        emit_expression(*call.arguments[1]) + ";");
            return;
        }
        if (name == "rx" || name == "ry" || name == "rz") {
            if (call.arguments.size() != 2) {
                unsupported(call, "invalid rotation arguments");
            }
            append_line(name + "(" + emit_expression(*call.arguments[1]) + ") " +
                        emit_expression(*call.arguments[0]) + ";");
            return;
        }
        unsupported(call, "call to '" + name + "'");
    }

    void emit_statement(const Statement& statement) {
        if (const auto* block = dynamic_cast<const BlockStatement*>(&statement)) {
            emit_block(*block);
            return;
        }
        if (const auto* variable = dynamic_cast<const VariableDeclaration*>(&statement)) {
            if (variable->varType == nullptr) {
                unsupported(statement, "an untyped variable");
            }
            if (is_qubit_type(*variable->varType) && indentation_ > 0) {
                unsupported(statement, "a block-local qubit");
            }

            std::string declaration = emit_type(*variable->varType) + " " + variable->name;
            if (variable->initializer != nullptr) {
                declaration.append(" = ").append(emit_expression(*variable->initializer));
            }
            append_line(declaration + ";");
            if (is_qubit_type(*variable->varType)) {
                append_line("reset " + variable->name + ";");
            }
            return;
        }
        if (const auto* expression = dynamic_cast<const ExpressionStatement*>(&statement)) {
            if (const auto* call =
                    dynamic_cast<const CallExpression*>(expression->expression.get())) {
                emit_call(*call);
            } else {
                append_line(emit_expression(*expression->expression) + ";");
            }
            return;
        }
        if (const auto* conditional = dynamic_cast<const IfStatement*>(&statement)) {
            append_line("if " + emit_condition(*conditional->condition) + " {");
            emit_branch(*conditional->thenBranch);
            if (conditional->elseBranch != nullptr) {
                append_line("} else {");
                emit_branch(*conditional->elseBranch);
            }
            append_line("}");
            return;
        }
        if (const auto* conditional = dynamic_cast<const TernaryStatement*>(&statement)) {
            append_line("if " + emit_condition(*conditional->condition) + " {");
            emit_branch(*conditional->thenBranch);
            if (conditional->elseBranch != nullptr) {
                append_line("} else {");
                emit_branch(*conditional->elseBranch);
            }
            append_line("}");
            return;
        }
        if (const auto* loop = dynamic_cast<const WhileStatement*>(&statement)) {
            append_line("while " + emit_condition(*loop->condition) + " {");
            emit_branch(*loop->body);
            append_line("}");
            return;
        }
        if (const auto* loop = dynamic_cast<const ForStatement*>(&statement)) {
            if (loop->initializer != nullptr) {
                emit_statement(*loop->initializer);
            }
            append_line("while " + emit_condition(*loop->condition) + " {");
            emit_branch(*loop->body);
            ++indentation_;
            append_line(emit_expression(*loop->increment) + ";");
            --indentation_;
            append_line("}");
            return;
        }
        if (const auto* reset = dynamic_cast<const ResetStatement*>(&statement)) {
            append_line("reset " + emit_expression(*reset->target) + ";");
            return;
        }
        if (const auto* measure = dynamic_cast<const MeasureStatement*>(&statement)) {
            append_line("measure " + emit_expression(*measure->qubit) + ";");
            return;
        }
        if (const auto* assignment = dynamic_cast<const AssignmentStatement*>(&statement)) {
            append_line(assignment->name + " = " + emit_expression(*assignment->value) + ";");
            return;
        }
        if (dynamic_cast<const EchoStatement*>(&statement) != nullptr) {
            return;
        }
        unsupported(statement, "this statement");
    }
};

}  // namespace

bool Qasm3Emitter::requires_structured_emission(const Program& program) noexcept {
    const FunctionDeclaration* main = find_main(program);
    return main != nullptr && main->body != nullptr && contains_control_flow(*main->body);
}

std::string Qasm3Emitter::emit(const Program& program) const { return Emitter{}.emit(program); }

}  // namespace bloch::compiler
