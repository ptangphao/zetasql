//
// Copyright 2019 ZetaSQL Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "zetasql/scripting/parsed_script.h"

#include <stack>

#include "zetasql/common/errors.h"
#include "zetasql/parser/ast_node_kind.h"
#include "zetasql/parser/parse_tree.h"
#include "zetasql/parser/parse_tree_errors.h"
#include "zetasql/parser/parse_tree_visitor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/flags/flag.h"
#include "zetasql/base/map_util.h"
#include "zetasql/base/canonical_errors.h"
#include "zetasql/base/status.h"
#include "zetasql/base/status_macros.h"

constexpr int kDefaultMaxNestingLevel = 50;
ABSL_FLAG(int, zetasql_scripting_max_nesting_level, kDefaultMaxNestingLevel,
          "Maximum supported number of nested scripting statements in a "
          "ZetaSQL script, such as BEGIN...END.");

namespace zetasql {

namespace {

// Visitor to verify the maximum depth of scripting nodes within the AST is
// within the allowable limit.  This check exists to ensure that deeply nested
// scripts will cleanly and determanistically, rather than potentially causing
// a stack overflow later, while traversing the tree.
class VerifyMaxScriptingDepthVisitor : public NonRecursiveParseTreeVisitor {
 public:
  zetasql_base::StatusOr<VisitResult> defaultVisit(const ASTNode* node) override {
    // We limit the nesting level of script constructs to a fixed depth,
    // so that which scripts can and cannot execute is stable across
    // implementation changes to either the script executor code or the
    // compiler.
    //
    // To make the depth checking more intuitive to the user, we don't increment
    // the depth counter for purely internal nodes, such as ASTStatementList.
    //
    // We also avoid incrementing the depth counter for expression nodes, in
    // order to maintain compatibility with pre-existing behavior for standalone
    // statements, for which expression depth is limited only by available stack
    // space.  As much as possible, we want any statement that is able to
    // execute without a script to execute successfully, within a script.
    if (node->IsScriptStatement()) {
      if (++depth_ > max_depth_) {
        return MakeSqlErrorAt(node) << "Script statement nesting level "
                                       "exceeds maximum supported limit of "
                                    << max_depth_;
      }
    }

    return VisitResult::VisitChildren(node, [this, node]() {
      if (node->IsScriptStatement()) {
        --depth_;
      }
      return zetasql_base::OkStatus();
    });
  }

 private:
  int depth_ = 0;
  const int max_depth_ =
      absl::GetFlag(FLAGS_zetasql_scripting_max_nesting_level);
};

// Visitor to verify that RAISE statements which rethrow an existing exception
// are used only within an exception handler.
class ValidateRaiseStatementsVisitor : public NonRecursiveParseTreeVisitor {
 public:
  ~ValidateRaiseStatementsVisitor() override {
    DCHECK_EQ(exception_handler_nesting_level_, 0);
  }

  zetasql_base::StatusOr<VisitResult> defaultVisit(const ASTNode* node) override {
    if (node->IsExpression() || node->IsSqlStatement()) {
      return VisitResult::Empty();
    }
    return VisitResult::VisitChildren(node);
  }

  zetasql_base::StatusOr<VisitResult> visitASTExceptionHandler(
      const ASTExceptionHandler* node) override {
    ++exception_handler_nesting_level_;
    return VisitResult::VisitChildren(node, [this]() {
      --exception_handler_nesting_level_;
      return zetasql_base::OkStatus();
    });
  }

  zetasql_base::StatusOr<VisitResult> visitASTRaiseStatement(
      const ASTRaiseStatement* node) override {
    if (node->is_rethrow() && exception_handler_nesting_level_ == 0) {
      return MakeSqlErrorAt(node)
             << "Cannot re-raise an existing exception outside of an exception "
                "handler";
    }
    return VisitResult::Empty();
  }

 private:
  int exception_handler_nesting_level_ = 0;
};

// Visitor to verify that:
// 1) Variable declarations occur only at the start of either a BEGIN block or
//    the entire script.
// 2) A variable is not declared which shadows a routine argument or another
//    variable of the same name in either the same scope, or any enclosing scope
class ValidateVariableDeclarationsVisitor
    : public NonRecursiveParseTreeVisitor {
 public:
  explicit ValidateVariableDeclarationsVisitor(
      const ParsedScript* parsed_script)
      : parsed_script_(parsed_script) {}

  zetasql_base::StatusOr<VisitResult> defaultVisit(const ASTNode* node) override {
    return VisitResult::VisitChildren(node);
  }

  static bool CanHaveDeclareStmtAsChild(const ASTNode* node) {
    const ASTStatementList* stmt_list = node->GetAsOrNull<ASTStatementList>();
    if (stmt_list == nullptr) {
      return false;
    }
    return stmt_list->variable_declarations_allowed();
  }

  zetasql_base::StatusOr<VisitResult> visitASTStatementList(
      const ASTStatementList* node) override {
    bool found_non_variable_decl = false;

    // Check for variable declaration outside of a block and for declaring
    // a variable that already exists.
    for (const ASTStatement* statement : node->statement_list()) {
      if (statement->node_kind() == AST_VARIABLE_DECLARATION) {
        if (found_non_variable_decl || !CanHaveDeclareStmtAsChild(node)) {
          return MakeVariableDeclarationError(
              statement,
              "Variable declarations are allowed only at the start of a "
              "block or script");
        }
        // Check for variable redeclaration.
        for (const ASTIdentifier* id :
             statement->GetAs<ASTVariableDeclaration>()
                 ->variable_list()
                 ->identifier_list()) {
          if (!zetasql_base::InsertIfNotPresent(&variables_, id->GetAsIdString(),
                                       id->GetParseLocationRange().start())) {
            return MakeVariableDeclarationError(
                id,
                absl::StrCat("Variable '", id->GetAsString(),
                             "' redeclaration"),
                absl::StrCat(id->GetAsString(), " previously declared here"),
                variables_[id->GetAsIdString()]);
          }
          if (zetasql_base::ContainsKey(parsed_script_->routine_arguments(),
                               id->GetAsIdString())) {
            return MakeVariableDeclarationError(
                id, absl::StrCat("Variable '", id->GetAsString(),
                                 "' previously declared as an argument"));
          }
        }
      } else {
        found_non_variable_decl = true;
      }
    }
    return VisitResult::VisitChildren(node, [this, node]() {
      // Remove variables declared in this block so that a subsequent block
      // can reuse the variable name.
      for (const ASTStatement* statement : node->statement_list()) {
        if (statement->node_kind() == AST_VARIABLE_DECLARATION) {
          for (const ASTIdentifier* id :
               statement->GetAs<ASTVariableDeclaration>()
                   ->variable_list()
                   ->identifier_list()) {
            variables_.erase(id->GetAsIdString());
          }
        }
      }
      return zetasql_base::OkStatus();
    });
  }

  zetasql_base::Status MakeVariableDeclarationError(const ASTNode* node,
                                            absl::string_view error_message) {
    return MakeSqlErrorAtNode(node, true) << error_message;
  }

  zetasql_base::Status MakeVariableDeclarationError(
      const ASTNode* node, const std::string& error_message,
      absl::string_view source_message,
      const ParseLocationPoint& source_location) {
    std::string script_text(parsed_script_->script_text());
    const InternalErrorLocation location = SetErrorSourcesFromStatus(
        MakeInternalErrorLocation(node),
        ConvertInternalErrorLocationToExternal(
            MakeSqlErrorAtPoint(source_location) << source_message,
            script_text),
        parsed_script_->error_message_mode(), script_text);
    return MakeSqlError().Attach(location) << error_message;
  }

  // Associates each active variable with the location of its declaration.
  // Used to generate the error message if the script later attempts to declare
  // a variable of the same name.
  absl::flat_hash_map<IdString, ParseLocationPoint, IdStringCaseHash,
                      IdStringCaseEqualFunc>
      variables_;

  const ParsedScript* parsed_script_;
};

// Visitor which records a mapping of each statement and elseif clause's index
// as a child of its parent.  This is used by the script executor implementation
// to locate the "next" statement or elseif clause after each node finishes
// running.
class PopulateIndexMapsVisitor : public NonRecursiveParseTreeVisitor {
 public:
  // Caller retains ownership of map_statement_index.
  explicit PopulateIndexMapsVisitor(
      ParsedScript::StatementIndexMap* map_statement_index,
      ParsedScript::ElseifClauseIndexMap* map_elseif_clauses)
      : map_statement_index_(map_statement_index),
        map_elseif_clauses_(map_elseif_clauses) {}

  zetasql_base::StatusOr<VisitResult> defaultVisit(const ASTNode* node) override {
    return VisitResult::VisitChildren(node);
  }
  zetasql_base::StatusOr<VisitResult> visitASTStatementList(
      const ASTStatementList* node) override {
    int index = 0;
    for (const ASTStatement* statement : node->statement_list()) {
      (*map_statement_index_)[statement] = index++;
    }
    return VisitResult::VisitChildren(node);
  }
  zetasql_base::StatusOr<VisitResult> visitASTElseifClauseList(
      const ASTElseifClauseList* node) override {
    int index = 0;
    for (const ASTElseifClause* clause : node->elseif_clauses()) {
      (*map_elseif_clauses_)[clause] = index++;
    }
    return VisitResult::VisitChildren(node);
  }

 private:
  ParsedScript::StatementIndexMap* map_statement_index_;
  ParsedScript::ElseifClauseIndexMap* map_elseif_clauses_;
};

// Visitor which builds a mapping, assocating each BREAK, CONTINUE, LEAVE, or
// ITERATE statement with its innermost loop.  Used to determine the target
// location of these statements, as well as any variable declaration blocks
// that must be exited when the statement executes.
class PopulateBreakContinueMapVisitor : public NonRecursiveParseTreeVisitor {
 public:
  explicit PopulateBreakContinueMapVisitor(
      ParsedScript::BreakContinueMap* break_continue_map)
      : break_continue_map_(break_continue_map) {}

  zetasql_base::StatusOr<VisitResult> defaultVisit(const ASTNode* node) override {
    return VisitResult::VisitChildren(node);
  }
  zetasql_base::StatusOr<VisitResult> visitASTBreakStatement(
      const ASTBreakStatement* node) override {
    ZETASQL_ASSIGN_OR_RETURN((*break_continue_map_)[node],
                     CreateBreakContinueContext(node));
    return VisitResult::Empty();
  }
  zetasql_base::StatusOr<VisitResult> visitASTContinueStatement(
      const ASTContinueStatement* node) override {
    ZETASQL_ASSIGN_OR_RETURN((*break_continue_map_)[node],
                     CreateBreakContinueContext(node));
    return VisitResult::Empty();
  }

 private:
  zetasql_base::StatusOr<BreakContinueContext> CreateBreakContinueContext(
      const ASTBreakContinueStatement* stmt) {
    const ASTLoopStatement* enclosing_loop = nullptr;
    std::vector<const ASTBeginEndBlock*> blocks_to_exit;
    for (const ASTNode* node = stmt->parent(); node != nullptr;
         node = node->parent()) {
      if (node->IsLoopStatement()) {
        enclosing_loop = node->GetAs<ASTLoopStatement>();
        break;
      }
      if (node->node_kind() == AST_BEGIN_END_BLOCK) {
        blocks_to_exit.push_back(node->GetAs<ASTBeginEndBlock>());
      }
    }
    if (enclosing_loop == nullptr) {
      return MakeSqlErrorAtNode(stmt, true)
             << stmt->GetKeywordText()
             << " is only allowed inside of a loop body";
    }
    return BreakContinueContext(enclosing_loop, std::move(blocks_to_exit));
  }

  ParsedScript::BreakContinueMap* break_continue_map_;
};

// Visitor to find the statement within a script that matches a given position.
class FindStatementFromPositionVisitor : public NonRecursiveParseTreeVisitor {
 public:
  explicit FindStatementFromPositionVisitor(const ParseLocationPoint& location)
      : location_(location) {}

  const ASTNode* match() const { return match_; }

  zetasql_base::StatusOr<VisitResult> defaultVisit(const ASTNode* node) override {
    if (match_ != nullptr) {
      return VisitResult::Empty();
    }
    if (node->IsStatement() || node->node_kind() == AST_ELSEIF_CLAUSE) {
      const ParseLocationRange& stmt_range = node->GetParseLocationRange();
      if (stmt_range.start() == location_) {
        match_ = node;
        return VisitResult::Empty();
      }
      return VisitResult::VisitChildren(node);
    }
    return VisitResult::Empty();
  }

  zetasql_base::StatusOr<VisitResult> visitASTExceptionHandlerList(
      const ASTExceptionHandlerList* node) override {
    return VisitResult::VisitChildren(node);
  }

  zetasql_base::StatusOr<VisitResult> visitASTExceptionHandler(
      const ASTExceptionHandler* node) override {
    return VisitResult::VisitChildren(node);
  }

  zetasql_base::StatusOr<VisitResult> visitASTElseifClauseList(
      const ASTElseifClauseList* node) override {
    return VisitResult::VisitChildren(node);
  }

  zetasql_base::StatusOr<VisitResult> visitASTStatementList(
      const ASTStatementList* node) override {
    return VisitResult::VisitChildren(node);
  }

  zetasql_base::StatusOr<VisitResult> visitASTScript(const ASTScript* node) override {
    return VisitResult::VisitChildren(node);
  }

 private:
  const ParseLocationPoint location_;
  const ASTNode* match_ = nullptr;
};
}  // namespace

zetasql_base::StatusOr<const ASTNode*> ParsedScript::FindScriptNodeFromPosition(
    const ParseLocationPoint& start_pos) const {
  FindStatementFromPositionVisitor visitor(start_pos);
  ZETASQL_RETURN_IF_ERROR(script()->TraverseNonRecursive(&visitor));
  return visitor.match();
}

zetasql_base::StatusOr<ParsedScript::VariableTypeMap>
ParsedScript::GetVariablesInScopeAtNode(const ASTNode* next_node) const {
  VariableTypeMap variables;

  for (const ASTNode* node = next_node->parent(); node != nullptr;
       node = node->parent()) {
    const ASTStatementList* stmt_list = node->GetAsOrNull<ASTStatementList>();
    if (stmt_list == nullptr || !stmt_list->variable_declarations_allowed()) {
      continue;
    }

    // Add variables declared in DECLARE statements up to, but not including,
    // the current statement.
    for (const ASTStatement* stmt : stmt_list->statement_list()) {
      if (stmt == next_node) {
        // Skip over DECLARE statements that haven't run yet when
        // <statement> is about to begin.
        break;
      }
      if (stmt->node_kind() == AST_VARIABLE_DECLARATION) {
        const ASTVariableDeclaration* decl =
            stmt->GetAs<ASTVariableDeclaration>();
        for (const ASTIdentifier* variable :
             decl->variable_list()->identifier_list()) {
          variables.insert_or_assign(variable->GetAsIdString(), decl->type());
        }
      } else {
        // Variable declarations are only allowed at the start of a block,
        // before any other statements, so no need to check further.
        break;
      }
    }
  }
  return variables;
}

zetasql_base::Status ParsedScript::GatherInformationAndRunChecksInternal() {
  // Check the maximum-depth constraint first, to ensure that other checks
  // do not cause a stack overflow in the case of a deeply nested script.
  VerifyMaxScriptingDepthVisitor max_depth_visitor;
  ZETASQL_RETURN_IF_ERROR(script()->TraverseNonRecursive(&max_depth_visitor));

  ValidateVariableDeclarationsVisitor var_decl_visitor(this);
  ZETASQL_RETURN_IF_ERROR(script()->TraverseNonRecursive(&var_decl_visitor));

  ValidateRaiseStatementsVisitor raise_visitor;
  ZETASQL_RETURN_IF_ERROR(script()->TraverseNonRecursive(&raise_visitor));

  // Walk the parse tree, constructing a StatementIndexMap, associating each
  // statement in the script with its index in the child list of the statement's
  // parent.  This is used to transfer control when advancing through the
  // script.
  PopulateIndexMapsVisitor populate_index_visitor(&statement_index_map_,
                                                  &elseif_clause_index_map_);
  ZETASQL_RETURN_IF_ERROR(script()->TraverseNonRecursive(&populate_index_visitor));

  // Walk the parse tree, building up a map, associating each BREAK, CONTINUE,
  // LEAVE, or ITERATE statement with its innermost loop and set of blocks that
  // must exit in order to continue or exit the loop.
  PopulateBreakContinueMapVisitor break_continue_map_visitor(
      &break_continue_map_);
  ZETASQL_RETURN_IF_ERROR(script()->TraverseNonRecursive(&break_continue_map_visitor));

  return zetasql_base::OkStatus();
}

zetasql_base::Status ParsedScript::GatherInformationAndRunChecks() {
  return ConvertInternalErrorLocationAndAdjustErrorString(
      error_message_mode(), script_text(),
      GatherInformationAndRunChecksInternal());
}

ParsedScript::ParsedScript(absl::string_view script_string,
                           const ASTScript* ast_script,
                           std::unique_ptr<ParserOutput> parser_output,
                           ErrorMessageMode error_message_mode,
                           ArgumentTypeMap routine_arguments)
    : parser_output_(std::move(parser_output)),
      ast_script_(ast_script),
      script_string_(script_string),
      error_message_mode_(error_message_mode),
      routine_arguments_(std::move(routine_arguments)) {}

zetasql_base::StatusOr<std::unique_ptr<ParsedScript>> ParsedScript::CreateInternal(
    absl::string_view script_string, const ParserOptions& parser_options,
    ErrorMessageMode error_message_mode, ArgumentTypeMap routine_arguments) {
  std::unique_ptr<ParserOutput> parser_output;
  ZETASQL_RETURN_IF_ERROR(ParseScript(script_string, parser_options, error_message_mode,
                              &parser_output));
  const ASTScript* ast_script = parser_output->script();
  std::unique_ptr<ParsedScript> parsed_script = absl::WrapUnique(
      new ParsedScript(script_string, ast_script, std::move(parser_output),
                       error_message_mode, std::move(routine_arguments)));
  ZETASQL_RETURN_IF_ERROR(parsed_script->GatherInformationAndRunChecks());
  return parsed_script;
}

zetasql_base::StatusOr<std::unique_ptr<ParsedScript>> ParsedScript::Create(
    absl::string_view script_string, const ParserOptions& parser_options,
    ErrorMessageMode error_message_mode) {
  return CreateInternal(script_string, parser_options, error_message_mode, {});
}

zetasql_base::StatusOr<std::unique_ptr<ParsedScript>> ParsedScript::CreateForRoutine(
    absl::string_view script_string, const ParserOptions& parser_options,
    ErrorMessageMode error_message_mode, ArgumentTypeMap routine_arguments) {
  return CreateInternal(script_string, parser_options, error_message_mode,
                        routine_arguments);
}

zetasql_base::StatusOr<std::unique_ptr<ParsedScript>> ParsedScript::Create(
    absl::string_view script_string, const ASTScript* ast_script,
    ErrorMessageMode error_message_mode) {
  std::unique_ptr<ParsedScript> parsed_script =
      absl::WrapUnique(new ParsedScript(script_string, ast_script, nullptr,
                                        error_message_mode, {}));
  ZETASQL_RETURN_IF_ERROR(parsed_script->GatherInformationAndRunChecks());
  return parsed_script;
}

}  // namespace zetasql
