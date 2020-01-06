// This file was autogenerated by binjs_generate_spidermonkey,
// please DO NOT EDIT BY HAND.
/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// To generate this file, see the documentation in
// js/src/frontend/binast/README.md.

#ifndef frontend_BinASTParser_h
#define frontend_BinASTParser_h

#include "mozilla/Maybe.h"
#include "mozilla/Variant.h"

#include "frontend/BCEParserHandle.h"
#include "frontend/BinASTParserPerTokenizer.h"
#include "frontend/BinASTToken.h"
#include "frontend/BinASTTokenReaderContext.h"
#include "frontend/BinASTTokenReaderMultipart.h"
#include "frontend/FullParseHandler.h"
#include "frontend/ParseContext.h"
#include "frontend/ParseNode.h"
#include "frontend/SharedContext.h"

#include "js/CompileOptions.h"
#include "js/GCHashTable.h"
#include "js/GCVector.h"
#include "js/Result.h"

namespace js {
namespace frontend {

template <typename Tok>
class BinASTParser : public BinASTParserPerTokenizer<Tok> {
 public:
  using Base = BinASTParserPerTokenizer<Tok>;

  using Tokenizer = Tok;

  using AutoList = typename Tokenizer::AutoList;
  using AutoTaggedTuple = typename Tokenizer::AutoTaggedTuple;
  using Chars = typename Tokenizer::Chars;
  using ListContext = typename BinASTTokenReaderBase::ListContext;
  using FieldContext = typename BinASTTokenReaderBase::FieldContext;
  using RootContext = typename BinASTTokenReaderBase::RootContext;
  using FieldOrRootContext = BinASTTokenReaderBase::FieldOrRootContext;
  using FieldOrListContext = BinASTTokenReaderBase::FieldOrListContext;

 public:
  // Auto-generated types.
  using AssertedDeclaredKind = binast::AssertedDeclaredKind;
  using BinaryOperator = binast::BinaryOperator;
  using CompoundAssignmentOperator = binast::CompoundAssignmentOperator;
  using UnaryOperator = binast::UnaryOperator;
  using UpdateOperator = binast::UpdateOperator;
  using VariableDeclarationKind = binast::VariableDeclarationKind;

 public:
  // BinASTParserPerTokenizer types.
  using AssertedScopeKind = typename Base::AssertedScopeKind;

 public:
  BinASTParser(JSContext* cx, CompilationInfo& compilationInfo,
               const JS::ReadOnlyCompileOptions& options,
               HandleScriptSourceObject sourceObject,
               Handle<LazyScript*> lazyScript = nullptr)
      : BinASTParserPerTokenizer<Tok>(cx, compilationInfo, options,
                                      sourceObject, lazyScript) {}
  ~BinASTParser() {}

 protected:
  // BinASTParserBase fields.
  using Base::cx_;

  using Base::alloc_;
  using Base::usedNames_;

  using Base::handler_;
  using Base::pc_;
  using Base::sourceObject_;

 protected:
  // BinASTParserPerTokenizer types.
  using AutoVariableDeclarationKind =
      typename Base::AutoVariableDeclarationKind;

 protected:
  // BinASTParserPerTokenizer fields.
  using Base::tokenizer_;
  using Base::variableDeclarationKind_;

 protected:
  // BinASTParserPerTokenizer methods.
  using Base::raiseEmpty;
  using Base::raiseError;
  using Base::raiseInvalidClosedVar;
  using Base::raiseInvalidKind;
  using Base::raiseInvalidVariant;
  using Base::raiseMissingDirectEvalInAssertedScope;
  using Base::raiseMissingField;
  using Base::raiseMissingVariableInAssertedScope;
  using Base::raiseOOM;

  using Base::buildFunctionBox;
  using Base::finishEagerFunction;
  using Base::finishLazyFunction;
  using Base::makeEmptyFunctionNode;
  using Base::setFunctionParametersAndBody;

  using Base::addScopeName;
  using Base::captureFunctionName;

  using Base::checkBinding;
  using Base::checkPositionalParameterIndices;
  using Base::getBoundScope;
  using Base::getDeclaredScope;

  using Base::checkClosedVars;
  using Base::checkFunctionLength;

  using Base::prependDirectivesToBody;

  using Base::forceStrictIfNecessary;

  using Base::isInvalidKindPossible;
  using Base::isInvalidVariantPossible;

 public:
  // ----- Sums of interfaces (by lexicographical order)
  // `ParseNode*` may never be nullptr
  JS::Result<Ok> parseAssertedMaybePositionalParameterName(
      AssertedScopeKind scopeKind,
      MutableHandle<GCVector<JSAtom*>> positionalParams,
      const ListContext& context);
  JS::Result<ParseNode*> parseAssignmentTarget(const FieldContext& context);
  JS::Result<ParseNode*> parseAssignmentTargetOrForInOfBinding(
      const FieldContext& context);
  JS::Result<ParseNode*> parseBinding(const FieldContext& context);
  JS::Result<ParseNode*> parseExpression(const FieldContext& context);
  JS::Result<ParseNode*> parseExpressionOrSpreadElement(
      const ListContext& context);
  JS::Result<ParseNode*> parseExpressionOrSuper(const FieldContext& context);
  JS::Result<ParseNode*> parseObjectProperty(const ListContext& context);
  JS::Result<ParseNode*> parseParameter(const FieldOrListContext& context);
  JS::Result<ParseNode*> parseProgram(const RootContext& context);
  JS::Result<ParseNode*> parsePropertyName(const FieldContext& context);
  JS::Result<ParseNode*> parseSimpleAssignmentTarget(
      const FieldContext& context);
  JS::Result<ParseNode*> parseStatement(const FieldOrListContext& context);
  JS::Result<Ok> parseSumAssertedMaybePositionalParameterName(
      const size_t start, const BinASTKind kind, AssertedScopeKind scopeKind,
      MutableHandle<GCVector<JSAtom*>> positionalParams,
      const ListContext& context);
  JS::Result<ParseNode*> parseSumAssignmentTarget(const size_t start,
                                                  const BinASTKind kind,
                                                  const FieldContext& context);
  JS::Result<ParseNode*> parseSumAssignmentTargetOrForInOfBinding(
      const size_t start, const BinASTKind kind, const FieldContext& context);
  JS::Result<ParseNode*> parseSumBinding(const size_t start,
                                         const BinASTKind kind,
                                         const FieldContext& context);
  JS::Result<ParseNode*> parseSumExpression(const size_t start,
                                            const BinASTKind kind,
                                            const FieldContext& context);
  JS::Result<ParseNode*> parseSumExpressionOrSpreadElement(
      const size_t start, const BinASTKind kind, const ListContext& context);
  JS::Result<ParseNode*> parseSumExpressionOrSuper(const size_t start,
                                                   const BinASTKind kind,
                                                   const FieldContext& context);
  JS::Result<ParseNode*> parseSumExpressionOrVariableDeclaration(
      const size_t start, const BinASTKind kind, const FieldContext& context);
  JS::Result<ParseNode*> parseSumObjectProperty(const size_t start,
                                                const BinASTKind kind,
                                                const ListContext& context);
  JS::Result<ParseNode*> parseSumParameter(const size_t start,
                                           const BinASTKind kind,
                                           const FieldOrListContext& context);
  JS::Result<ParseNode*> parseSumProgram(const size_t start,
                                         const BinASTKind kind,
                                         const RootContext& context);
  JS::Result<ParseNode*> parseSumPropertyName(const size_t start,
                                              const BinASTKind kind,
                                              const FieldContext& context);
  JS::Result<ParseNode*> parseSumSimpleAssignmentTarget(
      const size_t start, const BinASTKind kind, const FieldContext& context);
  JS::Result<ParseNode*> parseSumStatement(const size_t start,
                                           const BinASTKind kind,
                                           const FieldOrListContext& context);

  // ----- Interfaces (by lexicographical order)
  // `ParseNode*` may never be nullptr
  JS::Result<Ok> parseAssertedBlockScope(const FieldContext& context);
  JS::Result<Ok> parseAssertedBoundName(AssertedScopeKind scopeKind,
                                        const ListContext& context);
  JS::Result<Ok> parseAssertedBoundNamesScope(const FieldContext& context);
  JS::Result<Ok> parseAssertedDeclaredName(AssertedScopeKind scopeKind,
                                           const ListContext& context);
  JS::Result<Ok> parseAssertedParameterScope(
      MutableHandle<GCVector<JSAtom*>> positionalParams,
      const FieldContext& context);
  JS::Result<Ok> parseAssertedScriptGlobalScope(const FieldContext& context);
  JS::Result<Ok> parseAssertedVarScope(const FieldContext& context);
  JS::Result<ParseNode*> parseBindingIdentifier(
      const FieldOrListContext& context);
  JS::Result<ParseNode*> parseBlock(const FieldOrListContext& context);
  JS::Result<LexicalScopeNode*> parseCatchClause(const FieldContext& context);
  JS::Result<ParseNode*> parseDirective(const ListContext& context);
  JS::Result<ListNode*> parseFormalParameters(const FieldContext& context);
  JS::Result<Ok> parseFunctionExpressionContents(
      uint32_t funLength, ListNode** paramsOut, ListNode** bodyOut,
      const FieldOrRootContext& context);
  JS::Result<Ok> parseFunctionOrMethodContents(
      uint32_t funLength, ListNode** paramsOut, ListNode** bodyOut,
      const FieldOrRootContext& context);
  JS::Result<Ok> parseGetterContents(uint32_t funLength, ListNode** paramsOut,
                                     ListNode** bodyOut,
                                     const FieldContext& context);
  JS::Result<ParseNode*> parseIdentifierExpression(
      const FieldOrListContext& context);
  JS::Result<Ok> parseSetterContents(uint32_t funLength, ListNode** paramsOut,
                                     ListNode** bodyOut,
                                     const FieldContext& context);
  JS::Result<CaseClause*> parseSwitchCase(const ListContext& context);
  JS::Result<ParseNode*> parseSwitchDefault(const FieldContext& context);
  JS::Result<ParseNode*> parseVariableDeclarator(const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceArrayAssignmentTarget(
      const size_t start, const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceArrayBinding(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceArrayExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<Ok> parseInterfaceAssertedBlockScope(const size_t start,
                                                  const FieldContext& context);
  JS::Result<Ok> parseInterfaceAssertedBoundName(const size_t start,
                                                 AssertedScopeKind scopeKind,
                                                 const ListContext& context);
  JS::Result<Ok> parseInterfaceAssertedBoundNamesScope(
      const size_t start, const FieldContext& context);
  JS::Result<Ok> parseInterfaceAssertedDeclaredName(const size_t start,
                                                    AssertedScopeKind scopeKind,
                                                    const ListContext& context);
  JS::Result<Ok> parseInterfaceAssertedParameterScope(
      const size_t start, MutableHandle<GCVector<JSAtom*>> positionalParams,
      const FieldContext& context);
  JS::Result<Ok> parseInterfaceAssertedPositionalParameterName(
      const size_t start, AssertedScopeKind scopeKind,
      MutableHandle<GCVector<JSAtom*>> positionalParams,
      const ListContext& context);
  JS::Result<Ok> parseInterfaceAssertedScriptGlobalScope(
      const size_t start, const FieldContext& context);
  JS::Result<Ok> parseInterfaceAssertedVarScope(const size_t start,
                                                const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceAssignmentExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceAssignmentTargetIdentifier(
      const size_t start, const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceAwaitExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceBinaryExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceBindingIdentifier(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceBindingWithInitializer(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceBlock(const size_t start,
                                             const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceBreakStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceCallExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<LexicalScopeNode*> parseInterfaceCatchClause(
      const size_t start, const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceClassDeclaration(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceClassExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceCompoundAssignmentExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceComputedMemberAssignmentTarget(
      const size_t start, const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceComputedMemberExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceComputedPropertyName(
      const size_t start, const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceConditionalExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceContinueStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceDataProperty(const size_t start,
                                                    const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceDebuggerStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceDirective(const size_t start,
                                                 const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceDoWhileStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceEagerArrowExpressionWithExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceEagerArrowExpressionWithFunctionBody(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceEagerFunctionDeclaration(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceEagerFunctionExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceEagerGetter(const size_t start,
                                                   const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceEagerMethod(const size_t start,
                                                   const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceEagerSetter(const size_t start,
                                                   const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceEmptyStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceExpressionStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceForInOfBinding(
      const size_t start, const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceForInStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceForOfStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceForStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ListNode*> parseInterfaceFormalParameters(
      const size_t start, const FieldContext& context);
  JS::Result<Ok> parseInterfaceFunctionExpressionContents(
      const size_t start, uint32_t funLength, ListNode** paramsOut,
      ListNode** bodyOut, const FieldOrRootContext& context);
  JS::Result<Ok> parseInterfaceFunctionOrMethodContents(
      const size_t start, uint32_t funLength, ListNode** paramsOut,
      ListNode** bodyOut, const FieldOrRootContext& context);
  JS::Result<Ok> parseInterfaceGetterContents(const size_t start,
                                              uint32_t funLength,
                                              ListNode** paramsOut,
                                              ListNode** bodyOut,
                                              const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceIdentifierExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceIfStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceLabelledStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceLazyArrowExpressionWithExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceLazyArrowExpressionWithFunctionBody(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceLazyFunctionDeclaration(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceLazyFunctionExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceLazyGetter(const size_t start,
                                                  const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceLazyMethod(const size_t start,
                                                  const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceLazySetter(const size_t start,
                                                  const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceLiteralBooleanExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceLiteralInfinityExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceLiteralNullExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceLiteralNumericExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceLiteralPropertyName(
      const size_t start, const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceLiteralRegExpExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceLiteralStringExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceModule(const size_t start,
                                              const RootContext& context);
  JS::Result<ParseNode*> parseInterfaceNewExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceNewTargetExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceObjectAssignmentTarget(
      const size_t start, const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceObjectBinding(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceObjectExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceReturnStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceScript(const size_t start,
                                              const RootContext& context);
  JS::Result<Ok> parseInterfaceSetterContents(const size_t start,
                                              uint32_t funLength,
                                              ListNode** paramsOut,
                                              ListNode** bodyOut,
                                              const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceShorthandProperty(
      const size_t start, const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceSpreadElement(
      const size_t start, const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceStaticMemberAssignmentTarget(
      const size_t start, const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceStaticMemberExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceSuper(const size_t start,
                                             const FieldContext& context);
  JS::Result<CaseClause*> parseInterfaceSwitchCase(const size_t start,
                                                   const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceSwitchDefault(
      const size_t start, const FieldContext& context);
  JS::Result<ParseNode*> parseInterfaceSwitchStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceSwitchStatementWithDefault(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceTemplateExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceThisExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceThrowStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceTryCatchStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceTryFinallyStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceUnaryExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceUpdateExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceVariableDeclaration(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceVariableDeclarator(
      const size_t start, const ListContext& context);
  JS::Result<ParseNode*> parseInterfaceWhileStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceWithStatement(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceYieldExpression(
      const size_t start, const FieldOrListContext& context);
  JS::Result<ParseNode*> parseInterfaceYieldStarExpression(
      const size_t start, const FieldOrListContext& context);

  // ----- String enums (by lexicographical order)
  JS::Result<typename BinASTParser<Tok>::AssertedDeclaredKind>
  parseAssertedDeclaredKind(const FieldContext& context);
  JS::Result<typename BinASTParser<Tok>::BinaryOperator> parseBinaryOperator(
      const FieldContext& context);
  JS::Result<typename BinASTParser<Tok>::CompoundAssignmentOperator>
  parseCompoundAssignmentOperator(const FieldContext& context);
  JS::Result<typename BinASTParser<Tok>::UnaryOperator> parseUnaryOperator(
      const FieldContext& context);
  JS::Result<typename BinASTParser<Tok>::UpdateOperator> parseUpdateOperator(
      const FieldContext& context);
  JS::Result<typename BinASTParser<Tok>::VariableDeclarationKind>
  parseVariableDeclarationKind(const FieldContext& context);

  // ----- Lists (by lexicographical order)
  JS::Result<ParseNode*> parseArguments(const FieldContext& context);
  JS::Result<ListNode*> parseFunctionBody(const FieldContext& context);
  JS::Result<Ok> parseListOfAssertedBoundName(AssertedScopeKind scopeKind,
                                              const FieldContext& context);
  JS::Result<Ok> parseListOfAssertedDeclaredName(AssertedScopeKind scopeKind,
                                                 const FieldContext& context);
  JS::Result<Ok> parseListOfAssertedMaybePositionalParameterName(
      AssertedScopeKind scopeKind,
      MutableHandle<GCVector<JSAtom*>> positionalParams,
      const FieldContext& context);
  JS::Result<ListNode*> parseListOfDirective(const FieldContext& context);
  JS::Result<ListNode*> parseListOfObjectProperty(const FieldContext& context);
  JS::Result<ListNode*> parseListOfOptionalExpressionOrSpreadElement(
      const FieldContext& context);
  JS::Result<ListNode*> parseListOfParameter(const FieldContext& context);
  JS::Result<ListNode*> parseListOfStatement(const FieldContext& context);
  JS::Result<ListNode*> parseListOfSwitchCase(const FieldContext& context);
  JS::Result<ListNode*> parseListOfVariableDeclarator(
      ParseNodeKind declarationListKind, const FieldContext& context);

  // ----- Default values (by lexicographical order)
  JS::Result<ParseNode*> parseOptionalBinding(const FieldContext& context);
  JS::Result<ParseNode*> parseOptionalBindingIdentifier(
      const FieldContext& context);
  JS::Result<LexicalScopeNode*> parseOptionalCatchClause(
      const FieldContext& context);
  JS::Result<ParseNode*> parseOptionalExpression(const FieldContext& context);
  JS::Result<ParseNode*> parseOptionalExpressionOrSpreadElement(
      const ListContext& context);
  JS::Result<ParseNode*> parseOptionalExpressionOrVariableDeclaration(
      const FieldContext& context);
  JS::Result<ParseNode*> parseOptionalStatement(const FieldContext& context);
};

extern template class BinASTParser<BinASTTokenReaderContext>;
extern template class BinASTParser<BinASTTokenReaderMultipart>;

}  // namespace frontend
}  // namespace js

#endif  // frontend_BinASTParser_h
