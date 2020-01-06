// |reftest| skip error:SyntaxError -- class-fields-private is not supported
// Copyright (C) 2017 Valerie Young. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
esid: sec-scripts-static-semantics-early-errors
description: Early error when referencing privatename in constructor without being declared in class fields
info: |
  Static Semantics: Early Errors
    ScriptBody : StatementList

    It is a Syntax Error if AllPrivateNamesValid of StatementList with an empty List as an argument is false unless the source code is eval code that is being processed by a direct eval.

features: [class, class-fields-private]
negative:
  phase: parse
  type: SyntaxError
---*/

$DONOTEVALUATE();

class C {
  constructor() {
    this.#x;
  }
}
